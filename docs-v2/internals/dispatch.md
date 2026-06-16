# Dispatch — how a trigger becomes a run

**Shipped design-of-record.** Everything about turning an emitted frame into a
script run: the **trigger bus** (correlate frames into one inspection), the
**emit/fetch** model (address a frame-set by id, skip correlation), and
**dispatch groups** (give critical work its own threads + CPU priority).

---

## 1. Trigger bus — correlate frames into a run

A source plugin pushes frames with `host->emit_trigger(source, tid, ts, images,
n)`. Each `xi::ImageSource` instance has a TriggerBridge that tags emits with the
instance name. The bus groups frames sharing a 128-bit `tid` per the project's
`trigger_policy` and dispatches the script once per complete trigger:

| Policy | When it fires one inspection |
|---|---|
| `Any` | every emit fires immediately |
| `AllRequired` | wait for an emit from every named source under the same `tid` |
| `LeaderFollowers` | wait for the leader; attach the most-recent followers; fire on the leader |

Configured in `project.json: trigger_policy`. Instances don't know the policy —
they just emit; the bus correlates. The bus applies a drop policy
(oldest/newest/block) when its queue is full.

---

## 2. Emit/fetch — address a frame-set by id

Correlation is the wrong tool when **one emitter already holds a complete
frame-set** and just wants to run an inspection on it, *addressably* — for replay,
hot-param re-runs, or a downstream PLC send that needs **gap-free ordering**.
There's nothing to correlate; the run must be able to name the frame back; and the
*emitter*, not the core, must own the drop decision.

Three moves, keyed on one opaque **`res_id`**:

1. **Stage** — `emit_resource(emitter, res_id, images, n, json)` parks a frame-set
   (named images + a JSON metadata blob) under `res_id` in a per-emitter ring
   (`ResourceStore`). Addrefs each handle (caller may release after).
2. **Dispatch** — `emit_dispatch(emitter, res_id, ts)` builds an **id-only**
   `TriggerEvent` (no images), routes it by the emitter's *group*, and runs it —
   **bypassing bus correlation** (`set_dispatch_sink` → `enqueue_dispatch_`). The
   run carries `res_id` as its trigger id.
3. **Fetch** — inside that run the script reads the id back and pulls the frame:

   ```cpp
   auto r = xi::use("cam").fetch(xi::current_trigger().id_string());
   if (r.ok()) { xi::Image left = r.image("cam_left"); /* + r.data() metadata */ }
   ```
   Metadata is fetched eagerly; images lazily by key (pay only for what you read).

`xi::Emitter` (`xi_emitter.hpp`) wraps stage+dispatch and assigns a contiguous
`seq` so source authors get the contract right by default:
`em_.bind(host(), name()); em_.image("img", frame); if (!em_.emit()) { /* back-pressure */ }`.

### Identity vs. order

`res_id` is the **dispatch + fetch key** — opaque, unique per live frame
(`Emitter` uses the `seq`'s hex; a plugin may use a UUID); the store never reads
inside it. **Ordering is separate**: a downstream serialization point reads a
`seq` field carried *inside the JSON metadata*, injected by the emitter. Identity
(where) and order (when) are deliberately different axes.

### Back-pressure (never silent drop)

`emit_dispatch` returns **1 = accepted** or **0 = lane full / dispatch not
running**. The lane **rejects** when full (it owns no images, so a reject leaks
nothing) — it never silently drops. On a `0` the **emitter owns the choice**:
- **skip-before-burning-a-seq** — drop at the source, keep `seq_`, reuse the same
  `res_id`+`seq` next time. The dropped frame never enters the seq stream, so
  downstream stays **gap-free**. (`xi::Emitter::emit()` does exactly this.)
- or **retry** the same `res_id` later.

A gap-free `seq` stream is only possible because the thing that *assigns* `seq` is
the thing that *decides to drop*.

### Ring + lifetime

`ResourceStore` keeps a bounded per-emitter ring of recent `res_id`s (default 16);
past capacity the oldest evicts, re-emitting a `res_id` overwrites in place (so a
hot-param re-run reads the latest frame straight back). Image handles mirror the
bus: `emit_resource` addrefs; eviction / overwrite / `clear()` release;
`fetch_image` returns an addref'd handle the consumer releases. Never frees under
a live consumer, never leaks. Metadata is a JSON string copied into the entry.

### ABI surface (v2, additive; null-check)

| Function | Semantics |
|---|---|
| `emit_resource(emitter, res_id, images, n, json)` | Stage a frame-set; addref handles; overwrite same id; evict oldest past capacity. |
| `emit_dispatch(emitter, res_id, ts) → int` | Id-only run, routed by group, bypassing correlation. **1** accepted / **0** back-pressure. |
| `fetch_resource(emitter, res_id, buf, len) → int` | Read metadata JSON; returns length `L` (resize+retry if `L > len`), **-1** if not staged. No refcount change. |
| `fetch_image(emitter, res_id, key) → handle` | Lazily pull one image; **addref'd** handle (release it) or `XI_IMAGE_NULL`. Key `""` = single-image convention. |

Script-side via `xi::use(emitter).fetch(res_id)` → `xi::Resource`; host-side
backed by `ResourceStore`, wired by `install_resource_hooks()`.

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

## See also

- [`../reference/c-abi.md`](../reference/c-abi.md) — `emit_trigger` + the emit/fetch
  function signatures.
- [`../reference/instances.md`](../reference/instances.md) — TriggerBridge per source.
- `xi_trigger_bus.hpp` / `xi_resource_store.hpp` / `xi_emitter.hpp` — sources.
