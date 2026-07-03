# Dispatch — how an emit becomes a run

**Shipped design-of-record (ABI v6).** Turning an emitted record into a script
run: the **dispatch funnel** (one emit → one inspection) and **dispatch groups**
(give critical work its own threads + CPU priority).

---

## 1. The dispatch funnel — one emit, one run

A source plugin emits a **record** (images + metadata) with the SDK helper
`xi::emit_record(host, name, record)` → `host->emit_record(emitter, id, rec, ts)`.
The bus builds ONE `TriggerEvent` per emit and hands it to the worker; the script
reads it via `xi::current_trigger()`:

```cpp
auto t = xi::current_trigger();
if (!t.is_active()) return;            // false for synthetic timer ticks
auto img  = t.image("cam_left");       // image by the record's key
auto meta = t.meta();                  // routing/context metadata (Record)
auto id   = t.id_string();             // the emit id
```

There is **no correlation**: a source that wants several frames inspected
together puts them in the SAME record (a *gathering* source — see multi-camera
below). `id == XI_TRIGGER_NULL` asks the host to mint one. Per-image keys: a
single-image record keys by the emitter name (`t.image("<emitter>")`); a
multi-image record keys by the record's own keys (`t.image("cam_left")`).

### Trigger metadata (zero-serialize)

The record's JSON metadata (command id, recipe, lane hint…) rides the event **by
pointer** — a host-owned yyjson doc refcounted through the `DocRegistry` exactly
as image handles ride the `ImagePool`, so there's no serialize on the live path.
`t.meta()` returns a borrowed read-only `Record`; reads are free, a mutation
copy-on-writes into the script's own doc. `t.meta()` is total — an emit with no
metadata returns an empty `Record`.

### Multi-camera sync = a gathering plugin

Multi-camera synchronisation is **not** a bus policy (the old
`Any`/`AllRequired`/`LeaderFollowers` correlation was removed). Instead, ONE
gathering plugin grabs all cameras and emits a single record carrying every
frame: `xi::Record().image("cam_left", L).image("cam_right", R)` → one
`emit_record`. The frames are correlated because they ride the same record. See
the `synced_stereo` plugin and the `stereo_sync` example.

### Replay / hot-param re-run = a buffer-replay plugin

Record/replay is **not** a host facility either. A buffer-replay plugin captures
records (via its `process()`) into a ring and re-emits them with `emit_record` on
demand — that's the HDevelop-style "tune a Param, re-inspect the same frame"
loop. See `examples/buffer_replay_demo` — its `inspect.cpp` drives the `cache`
reference plugin (instance `buffer`) as the replay ring, with `pulse_src` as the
live source.

### Headless injection

A test/tool drives dispatch without a live source two ways: `cmd:run` injects a
record host-side, and `cmd:exchange_instance` drives a plugin directly (e.g. a
source's `fire` command). See `reference/ws-protocol.md`.

---

## 2. (removed) Trigger-bus correlation + emit/fetch + recorder

The ABI-v6 dispatch cleanup removed three subsystems that earlier versions had:
the trigger-bus **correlation policies** (`trigger_policy`,
`Any`/`AllRequired`/`LeaderFollowers`), the **emit/fetch** stage-and-dispatch-by-id
model (`emit_resource`/`fetch_resource`/`fetch_image`/`emit_dispatch` +
`xi::use().fetch()`), and the host **record/replay** recorder
(`recording_start/stop/replay`). Multi-camera sync, addressable re-runs, and
replay are all plugin composition now (gathering + buffer-replay plugins, §1).
The legacy section that documented them is gone; this note is a tombstone for
anyone following an old link.

---

## 3. Dispatch groups — priority + concurrency

**Status: v1 shipped, gated on `parallelism.groups`** (legacy = one implicit
group, unchanged). Some trigger work is latency-critical (a go/no-go gate), some
best-effort (heavy analytics). Groups give the critical class its own threads at a
higher CPU priority so it's never starved.

**One model: group-owned worker threads.** Each group spawns its own
`max_parallel` workers at its `thread_priority` and they pull only from that
group's FIFO queue. Consequences:
- **No shared pool, no cross-group scheduler, no priority integer.** Groups never
  contend for a slot (each owns its own); total threads = `Σ max_parallel`
  (derived — can't be mis-sized). The only arbiter is **OS thread priority** on
  the cores (Windows + Linux are priority-preemptive).
- **`max_parallel`** bounds a group's footprint (a bursty group can't thrash all
  cores); **`thread_priority`** (`high`/`normal`/`low`) decides who wins a scarce
  core. Default `high(4, normal)` / `low(1, below-normal)` — priority without a
  rank integer.
- **Deterministic capacity.** `high` always has its 4 workers even when `low` is
  idle; strict partitioning gives the critical path predictable latency (no
  borrowing — that would reintroduce a shared pool).

**Residual coupling** (decoupling isn't total): a **shared non-reentrant
instance** serializes callers → possible priority inversion (don't share an
instance across priority groups, or mark it `reentrant`); and shared ImagePool /
memory bandwidth / disk aren't arbitrated by thread priority.

Cost: a group holds its N threads even when idle (a blocked worker ≈ a stack, no
CPU) — fine for a handful of priority classes; dozens of groups would want a
shared pool instead, but that's not the use case.

## 4. Ordered output — `result_order` + staged sinks

Under a parallel pool (`parallelism.dispatch_threads > 1`, or a group's
`max_parallel > 1`) workers finish out of frame order, so emission has two gates
that re-impose order without throttling compute:

- **`result_order`** (per-pool / per-group) governs the per-frame **result**
  stream (`run_result` / `run_*`). `"completion"` (default) emits as each worker
  finishes — lowest latency, out-of-order on the wire. `"arrival"` claims a gapless
  seq at dequeue (so it tracks FIFO arrival) and an EmitTurn gate replays emission
  in that order; compute still runs fully parallel, only emission is serialized, and
  `run_id` is monotonic on the wire.
- **Ordered output sinks.** A plugin whose `plugin.json` declares `"sink": true`
  (alias `"role": "sink"`) is not called inline during inspect; the host **stages**
  each `use(name).process(rec)` and **flushes** them after the inspect, inside the
  same arrival-order gate, so deliveries land in **frame-arrival order** under
  `dispatch_threads > 1`. Each flushed record is stamped with the reserved key
  `"$seq"` == the wire `run_id`, so the sink can correlate its packet to the frame.
  The `expose` plugin is a sink (live output never tears/reorders across workers).
  See the `sink` row in [`../reference/c-abi.md`](../reference/c-abi.md) and the
  *Parallel dispatch* section of
  [`../guides/write-a-script.md`](../guides/write-a-script.md).

## 5. The pack plane on dispatch — dual carry + staged pack push

**Status: shipped (polaris2 wave-2 / U3), transitional until THE CUT.** Dispatch
carries **two data currencies** on one machinery:

- **Dual-carry `TriggerEvent`.** Every event carries the Record payload (image
  map + refcounted meta doc) *and* an optional sealed-pack handle
  (`TriggerEvent::pack`, `XI_PACK_NULL` for Record-era events). A pack source
  emits via `emit_pack(emitter, id, pack, ts)` (the `xi_pack_v1` verb — same
  funnel discipline as `emit_record`: one emit, one run); the bus stores the
  sealed handle on the event and extracts no images — the pack *is* the
  payload. Ordering keys on `id + arrival_id` identically for both currencies.
  The worker hands the pack to the script through the trigger view; the SDK
  `Trigger` takes its own retain, so `t.pack()` (a `ScriptPack`) is valid
  however long the script holds it while `t.image()`/`t.meta()` keep serving
  the Record side.
- **Staged pack push / flush.** The §4 ordered-sink discipline applies to packs
  unchanged: `xi::use(sink).push(pack)` on a `"sink": true` target is staged
  (the host retains the pack onto the staged emit) and flushed after the
  inspect inside the same arrival-order gate, so pack deliveries land in frame
  order under parallel dispatch; a non-sink target is pushed inline. One
  asymmetry vs the Record path: a sealed pack is **immutable**, so the host
  cannot stamp `"$seq"` at flush time — the producer stamps it before seal
  (`b.add_i64("$seq", (int64_t)xi::run_id())`). `use(name).process(pack)` is
  the request-reply sibling: a `$fault` input short-circuits (the host mints
  the propagated fault pack without entering the plugin), and a sink target is
  refused (rc −5) — feed sinks via `push()`.

Container, registry, fault contract and ingress:
[`pack-plane.md`](./pack-plane.md).

## See also

- [`../reference/c-abi.md`](../reference/c-abi.md) — the `emit_record` function
  signature.
- [`../reference/instances.md`](../reference/instances.md) — per-source instances.
- [`pack-plane.md`](./pack-plane.md) — the v3 pack currency riding this dispatch.
- `xi_trigger_bus.hpp` — source.
