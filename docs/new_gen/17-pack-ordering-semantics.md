# 17 — Ordered-sink semantics on the pack plane (U3)

> [2026-07-11] Partially superseded: the `xi_script_set_run_id` / `set_run_context` ambient-TLS mechanism described below was replaced by the explicit per-run RunContext (commit `a293cfe`); the ordering contract ($seq = xi::run_id(), producer-stamped before seal) is unchanged — see docs/reference/c-abi.md.

Status: **DECIDED + LANDED** (2026-07-03). Owner: U3 (CUT-GATE item).
Closes Gate P2 residual #2 (doc 10 §Gate P2) and flips matrix row C3 (doc 12).

The question U3 asked: the Record path's ordered sinks got `$seq` **stamped by
the host** into the staged record at flush; a sealed pack is immutable, so the
host cannot stamp. Who owns ordering identity on the pack plane — and what may
a sink assume? Plus the documented v0 gap: `use(name).process(pack)` on a sink
target ran INLINE, landing side effects in completion order under parallel
dispatch.

## The contract

Two separable concerns, deliberately given two separate carriers:

### 1. Delivery ORDER is envelope-carried and authoritative

For a declared ordered sink (plugin.json `"sink": true`), `use(sink).push(pack)`
is **staged** on the worker (`use_push_pack_cb` → `StagedEmit`,
`backend/src/service_sinks.cpp`) and **flushed after the inspect inside the
EmitTurn gate** (`flush_staged_emits_`), exactly like the Record path's staged
`use(sink).process(rec)`. What the sink's `xi.pack@1` door may therefore
assume, with **no cooperation from the producer and no pack entry required**:

- Under `parallelism.result_order: "arrival"`, door invocations arrive in
  **frame-arrival order** (the emit gate replays flushes in the order run ids
  were claimed at dequeue). Under `"completion"` they arrive in completion
  order — the same policy knob, same semantics, as every other emission.
- Within one frame, multiple pushes arrive in **script call order**
  (`g_staged` is a per-thread vector, flushed front to back).
- Door invocations for one lane are **serialized** (the gate admits one
  frame's flush at a time) — a sink never sees two frames' staged pushes
  concurrently from the same lane.
- **Fail-closed**: a crashed/aborted inspect and a stop-wake **drop** the
  staged pushes (`StagedEmitGuard` / the `my_turn == false` skip) — a sink
  never receives a torn frame out of order, it receives nothing.

The staging envelope carries delivery order and **only** delivery order. It
does **not** backfill identity: when a pack has no `$seq` entry, the sink door
still gets the pack in the right order but with its content exactly as sealed
(expose then lifts channel `"default"`, seq `0` — honest absence, not a host
guess).

### 2. In-band IDENTITY is producer-stamped — blessed as the ONE mechanism

`$channel` / `$seq` are ordinary pack **entries**, stamped by the producer
**before seal** (`ScriptPackBuilder`, or a plugin building its emit pack).
This is now the blessed, only, in-band ordering/correlation mechanism on the
pack plane:

- The host **never** stamps, rewrites, splices, or rebuilds a sealed pack on
  the push path. Byte-identity is contractual:
  `push(pack)` wire dump ≡ direct host-side dump ≡ disk dump
  (memory ≈ wire ≈ disk, doc 07; proven in `expose_script_push_test`,
  `record_save_pack_test`, `record_replay_pack_test`).
- **No precedence rule is needed** because there is no second mechanism: the
  entry is the only in-band carrier; the envelope (above) is order-only and
  carries no injectable identity.
- Canonical profile applies (everything that lands in entries is
  canonical-gated at seal — `ScriptPackBuilder`/`xi::mp::canonicalize`).

### 3. The host-truth bridge: `xi::run_id()`

What the Record host-stamp actually injected was the **arrival/run id**
(claimed at dequeue, gapless per lane, monotonic on the wire in arrival mode).
Key fact making producer stamping lossless: **that id exists before compute
starts** — `run_one_inspection` receives it, and the emit turn was claimed on
it, before the script runs. So the host surfaces it to the script instead of
splicing it into sealed bytes:

- New per-run context field, set/cleared by the host around every inspect
  exactly like `frame_path`: optional script export `xi_script_set_run_id`
  (`xi_script_support.hpp`), host call sites in `service_inspect.cpp` beside
  `set_run_context`.
- Script accessor **`xi::run_id()`** (`xi_io.hpp`): the arrival/run id of the
  inspection this thread is computing; `0` outside an inspect. Same
  thread-local discipline as `current_frame_path()` — read it on the inspect
  thread; capture by value into `xi::async` / `parallel_for` bodies (the
  trigger-snapshot rule).
- A producer that wants qa_result_order's host-stamped-arrival-id semantics
  writes `add_i64("$seq", xi::run_id())` before seal — the **same value** the
  Record path's flush-time stamp injected, now stamped where identity belongs:
  by the producer, into content, before seal.

Back-compat: separate optional symbol (the established pattern — leader/meta/
push callbacks). Older host + new script → `xi::run_id()` returns 0. New host
+ older script → export absent, host skips the call. Nothing breaks.

### Rejected alternatives (recorded so the cut doesn't re-litigate)

- **Host splice at flush** (rebuild the pack with `$seq` injected — arena
  splice is cheap): breaks seal-immutability doctrine, breaks the push ≡ dump
  byte-identity contract above, silently diverges wire bytes from what the
  script sealed (and from what record_save persists), and reintroduces the
  shared-payload double-stamp class that the Record path needed COW to paper
  over (doc 12 C4 note) — the class sealed packs made unrepresentable.
- **Door-args ABI** (pass the envelope's run id alongside the pack into
  `xi.pack@1`): new ABI surface on every pack door for one integer, and it
  creates the dual-mechanism precedence question ("entry says 7, door-arg says
  9") forever. The run id is available BEFORE seal, so the producer can put it
  in-band; out-of-band delivery order needs no number at all.

## (b) `use(sink).process(pack)` — RULED an anti-pattern, rejected fail-loud

Decision: a `process(pack)` call on a **declared ordered sink** target is
**rejected at call time** with a new return code **`-5`** — it no longer runs
inline (the v0 completion-order gap is closed by making the case
unrepresentable, not by staging it). Script-side, `use().process(ScriptPack)`
maps `-5` to an empty pack plus a **once-per-name error log** naming the fix:
use `xi::use(name).push(pack)`.

Why reject rather than stage (the Record path stages):

- `process()` is the **request-reply** surface — its whole point is the
  returned pack. A staged call's reply **cannot exist** until the flush, after
  the script has moved on; staging would force every sink-target `process()`
  to return an empty pack that is indistinguishable from the documented "door
  hard failure" empty at every call site. That is a silent semantic fork on
  the target's declared role — exactly the failure class this repo rejects
  (fail-loud, honest results).
- The Record path staged `process(rec)`-on-sink because Record **has no
  push()** — the fire-and-forget feed and the request-reply call were forced
  through one verb. The pack plane separates them: `push()` **is** the sink
  feed (staged, ordered, fire-and-forget, ack dropped host-side); `process()`
  is the door chain. One verb per job; the doctrine matches the post-cut end
  state (`use(sink).process(rec)` retires with Record).
- Nothing observes the old inline behavior (doc 12 C3: "neither is exercised
  by any example today"); `cache` — the one `process(pack)`-fed door pattern
  (E1) — is not a declared sink, so capture-to-cache is untouched.

Return-code table for `use_pack_process_cb` after this change:
`0` ok · `-1` no such instance · `-2` door crashed · `-3` quarantined ·
`-4` no pack door · **`-5` declared ordered sink — use push()**.
(`-5` is checked before the fault gates: misuse is static, the plugin is never
entered, no health/quarantine state is touched. The runner's mirror
(`runner_main.cpp`) carries the same branch for contract symmetry — inert
there, the runner never marks adapters as sinks.)

> **rc-namespace note (cross-ref doc 14):** host-funnel return codes are
> PER-VTABLE namespaces, not one global enum. `-5` means **sink-target
> rejection** on this use-door funnel and **reentrancy** on `xi.cap.call`
> (the capability funnel, doc 14). The two tables are independent and each
> is frozen in its own doc — never cross-read a code from one funnel
> against the other's table.

`push()` on a **non-sink** pack-door target stays inline ("deliver now") —
the ordering guarantee attaches to the declared sink role, not to the verb.

## What a sink author should read off this

| You want | You do |
|---|---|
| Side effects in frame order under parallel dispatch | declare `"sink": true`; be fed via `push()`; trust door invocation order — nothing else required |
| Per-frame identity on the wire / on disk | require producers to stamp `$seq` (host truth: `$seq = xi::run_id()`); treat absence as absence (0), never guess |
| A reply the script can read | you are not a sink — don't declare `"sink": true`; be driven via `process()` (inline, request-reply) |

## Evidence

- **`toolbox/pack_order_gate_test.cpp`** (new, plugins ctest): 4 worker
  threads, uneven compute, real `xi::EmitGate`/`EmitTurn` + the real expose
  DLL door, service staging discipline mirrored. Asserts: gated flush →
  door-observed wire seq exactly `1..N` in arrival order (zero inversions,
  within-frame call order preserved); ungated control on the same workload →
  inversions > 0 (the workload genuinely reorders); pack/pool registries
  balance to zero.
- **`toolbox/use_pack_door_test.cpp`** (extended): a sink-declared adapter
  rejects `process(pack)` with `-5` → script sees an empty pack, the sink door
  was never entered; `push()` to the same adapter still lands.
- **`qa/qa_pack_order/`** (new, live service — the pack-only successor
  to `qa_result_order`, flips matrix row C3): same uneven-timing workload,
  `dispatch_threads=4`, script builds a pack per frame with
  `$seq = xi::run_id()` and pushes to expose; also drives
  `use("expose").process(probe)` once per frame and records the rejection in
  the pushed pack. Driver asserts: arrival mode → wire seq strictly increasing
  (zero inversions); completion mode → inversions > 0; `process`-on-sink
  rejected on every frame.
- Byte-identity (unchanged, re-relied-upon): `expose_script_push_test`.

## Doc deltas

- doc 12 row C3 → GREEN (this doc + `qa_pack_order`); scorecard re-tallied;
  §Unscheduled U3 marked resolved.
- doc 10 Gate P2 residual #2 (U3) → closed, pointer here.
