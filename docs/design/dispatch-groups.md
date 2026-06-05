# Dispatch groups — priority + concurrency for trigger work (design)

> **Status: v1 implemented (gated lane dispatcher).** The core model below is
> built and gated on `parallelism.groups`; legacy (no groups) is unchanged. See
> the Increment plan for what shipped vs what's deferred. Assessment 2026-06-04.

## The problem

A project has one inspection script reached by many triggers (per source). Some
trigger work is **latency-critical** (a go/no-go gate, an actuator) and some is
**best-effort** (heavy analytics, archival) that may lag. Today the dispatcher is
a **single FIFO queue + worker pool** (`g_ev_queue` + `spawn_dispatch_pool_`,
`parallelism.dispatch_threads`): every trigger competes equally, no priority.

We want to group trigger work and give each group a concurrency cap + a CPU
priority, so the critical group is never starved by the best-effort group.

## The key insight (what makes this simple)

If the worker pool is sized **`dispatch_threads ≥ Σ(group.max_parallel)`**, then
every group always has a free worker slot when its trigger arrives (bounded only
by its own `max_parallel`). We get this **for free by giving each group its own
threads** (total = `Σ max_parallel`) — so it's a structural guarantee, not a knob
you can mis-set. **Groups are then decoupled at the dispatch layer** — no group
ever waits for another's slot. What remains is pure **compute-resource
occupancy**, and the OS handles that:

- **OS thread priority does real preemption on the CPU.** Run the low-priority
  group's workers at a lower OS thread priority; when cores are scarce the OS
  preempts low-priority threads for high-priority ones (Windows + Linux are
  priority-preemptive). We *can't* preempt a running inspect off its worker slot,
  but with enough slots we don't need to — and the OS preempts at the core level.
- **`max_parallel` bounds each group's footprint** so a bursty best-effort group
  can't spawn 100 concurrent inspects and thrash memory/cores.

So the "scheduler" is trivial: each group owns its threads + a FIFO queue; there
is **no shared pool and no cross-group priority queue**.

### Residual coupling (the decoupling is not total)

"Only compute occupancy" holds **only if groups use disjoint resources**. Still
couples them:

1. **A shared non-reentrant plugin instance** — its lock serializes callers, and a
   low-pri inspect holding it blocks a high-pri one = **priority inversion**.
   Mitigation: don't share an instance across priority groups (or mark it
   `reentrant`). We can't do RTOS priority-inheritance on a plain mutex easily.
2. **Shared ImagePool / memory bandwidth / disk-I/O** — a heavy group can pressure
   these; thread priority doesn't arbitrate memory bandwidth.

## One model: group-owned worker threads

There is **one** model, not two: **each group spawns its own `max_parallel`
worker threads**, sets them to its `thread_priority` at spawn, and those workers
pull only from that group's queue. Consequences:

- **No shared pool, no cross-group scheduler, no `priority` integer.** Groups never
  contend for a worker slot (each owns its own), so "which group gets the next
  freed slot" never arises. The only thing that arbitrates is the **OS thread
  priority** on the CPU cores.
- **"Threads sufficient" is structural, not a setting.** Total worker threads =
  `Σ group.max_parallel`, derived. You can't mis-size it; there's no
  `dispatch_threads` to under-provision.
- **Deterministic per-group capacity.** `high` always has exactly its 4 workers,
  even when `low` is idle. We deliberately *don't* let `high` borrow `low`'s idle
  thread — strict partitioning gives the critical path predictable latency, which
  matters more than squeezing extra throughput. (Borrowing would reintroduce a
  shared pool + scheduler; not worth it.)
- **Group priority = `thread_priority` + `max_parallel`.** `thread_priority` decides
  who wins a core when cores are scarce; `max_parallel` is how many cores a group
  may use at once. The default high(4, normal) / low(1, below-normal) encodes the
  priority difference with no integer rank.
- **Legacy = one implicit group.** A project with no `groups` is exactly one group
  owning `dispatch_threads` workers — today's behaviour, same code path.

> Cost: a group with `max_parallel: N` always holds N threads even when idle (a
> blocked worker costs ~a stack, no CPU). Fine for a handful of priority classes
> (critical / best-effort); if you ever needed dozens of groups, a shared pool +
> scheduler would be more thread-efficient — but that's not the use case.

## Group parameters

| Param | Meaning |
|---|---|
| `name` | group id; triggers carry it |
| `max_parallel` | number of worker threads this group owns = max concurrent inspects |
| `thread_priority` | OS thread priority of this group's workers: `high` / `normal` / `low` (→ `THREAD_PRIORITY_ABOVE_NORMAL` / `NORMAL` / `BELOW_NORMAL`) |
| `queue_depth` + `overflow` | per-group buffering (`drop_oldest` for live, `block`/deep for archival) |
| `min_interval_ms` | rate limit: dispatch this group at most once per N ms; surplus triggers coalesce → keep latest (the *deliberate* way to "reduce frequency", vs sleeping a worker) |

There is intentionally **no** `priority` / `weight` / oversubscription knob — those
only exist to arbitrate a *shared* pool, which this model doesn't have.

### Why not just `sleep()` in a low-priority inspect?

A `sleep` does yield the **CPU** (OS reschedules), and under "enough slots" it
won't starve high-pri for a slot — but it's the wrong tool:

- It still **holds the worker slot** (busy-but-idle) and any locks it grabbed —
  **never sleep while holding a shared instance lock** (instant priority inversion).
- For "reduce frequency" it only works as a *side effect*: the slow worker lets the
  queue fill and `overflow` drops frames — wasteful (frames enqueued then dropped)
  and adds stale-frame latency.

`min_interval_ms` (deliberate rate limit, frees the worker) + `thread_priority`
(OS-level CPU yield, no wasted slot) achieve both intents cleanly. `sleep` stays a
legitimate quick escape hatch with the lock caveat.

## Default configuration

Two groups out of the box (what the scaffold ships / the recommended start):

```jsonc
"parallelism": {
  "default_group": "high",
  "groups": [
    { "name": "high", "max_parallel": 4, "thread_priority": "normal",
      "queue_depth": 8,  "overflow": "drop_oldest" },
    { "name": "low",  "max_parallel": 1, "thread_priority": "low",
      "queue_depth": 50, "overflow": "drop_oldest" }
  ]
}
```

- **high**: owns 4 worker threads at normal OS priority — the critical path.
- **low**: owns 1 worker thread at below-normal OS priority — best-effort; can lag,
  yields the CPU to `high` when cores are scarce, never touches high's threads.
- Total worker threads = 4 + 1 = 5, **derived** (no `dispatch_threads` knob).

### Optional per-group `cpu_affinity`

A group may pin its workers to a set of cores. Default (absent) = **unbound** (the
OS schedules freely — recommended unless you have a specific reason). The value is
a **mask**, not a single core — a worker may run on *any* core in its set:

```jsonc
{ "name": "critical", "max_parallel": 4, "thread_priority": "high",
  "cpu_affinity": [0,1,2,3] }          // all 4 workers may use cores 0-3
```

- `[0,1,2,3]` — one shared mask: every worker in the group may run on any of 0-3.
- `[[0,1],[2,3]]` — per-worker masks: worker 0 → {0,1}, worker 1 → {2,3}, … (still
  multi-core each). Fewer masks than workers → wraps (`set[i % N]`).
- `[3]` — pin to a single core (the special case).

Bogus core ids are intersected with the process's allowed mask (a bad config can't
wipe affinity to nothing). `> 64` cores needs Windows processor groups — TODO.
Win32 `SetThreadAffinityMask`; `// TODO(linux)` `pthread_setaffinity_np`.
**Caveat:** affinity is a sharp tool — pinning more threads than cores, or fighting
the scheduler, usually *hurts*. Use it to isolate a critical group on dedicated
cores (and keep other groups off them), not as a default.

**Backward compatibility:** if `parallelism.groups` is absent, the dispatcher
stays exactly as today — **one implicit group** owning `dispatch_threads` workers
(legacy `dispatch_threads`/`queue_depth`/`overflow` map onto that single group).
Grouping is opt-in; existing projects are unaffected.

## Assigning a trigger to a group

A `TriggerEvent` gains a `group` field. The **emitting source instance declares
it** in its `instance.json`:

```json
{ "plugin": "mock_camera", "group": "high" }
```

The backend resolves source→group on `emit_trigger` (untagged → `default_group`).
Chosen over a project-level source→group map or an `emit_trigger` arg because the
source is the natural owner of "how urgent is my stream".

### The synthetic timer tick loads the default group

Continuous mode also runs a **timer thread** that emits a synthetic empty tick at
the `cmd:start` fps so source-less scripts still dispatch. That tick is *untagged*,
so it routes to `default_group` — i.e. the default group carries `fps` extra
dispatches/sec on top of any real source assigned to it. Near saturation this is
enough to tip *only the default group* into backlog + drops while the others stay
healthy (a real observation: 8 groups each fed 20/s at ~21.6/s capacity — the
default group, also taking the timer, was the one that piled up). This is the
per-group isolation working (the overload stays contained), but it means: **don't
put a near-saturated source on the default group**, give the default group
headroom, or run **trigger-only**.

**Trigger-only mode:** `cmd:start {"fps": 0}` (or `--autostart-fps=-1`) starts
continuous mode — lanes spawn, sources route — with **no timer thread at all**. Use
it whenever the project has its own sources and you don't want synthetic ticks
skewing the default group. `fps > 0` keeps the legacy timer.

## Result ordering with groups

> **Per-group `result_order` shipped.** Each lane owns its own emit-sequence gate
> (`EmitGate`); `result_order: "arrival"` on a group serialises *that group's*
> emission by frame-arrival order without touching any other group. The legacy
> single pool keeps its one global gate, unchanged. Regression: `qa_dispatch_groups`
> Test E (arrival → 0 run_id inversions; a completion control proves the workload
> scrambles). What's still pending: the `group` wire tag on `vars`/`run_finished`
> (only `run_result` carries `group` today) — see item 2 below.

The legacy `result_order` is **global**: `arrival` mode gates emission by one
dispatch sequence so the `vars` / `run_finished` stream is in frame-arrival order.
With groups that single global order **breaks the whole point**:

- The critical group's output would be gated waiting for a *lagging best-effort
  frame's* turn — i.e. the low group could stall high's output ordering.
- Multiple groups emit onto the one WS stream, so results **interleave** and a
  consumer can't tell which group a `vars` message came from.

So ordering becomes **per-group**, not global:

1. **`result_order` is per-group** ✅ shipped — each group has its own
   `arrival`/`completion` setting and its **own emit-sequence gate** (`GroupLane::gate`,
   seq claimed at dequeue under the lane lock). A slow `low` group can never delay
   `high`'s in-order emission.
2. **Every emitted message carries its `group`** (+ a per-group sequence) —
   `run_result` already carries `group`; `vars` / `run_started` / `run_finished`
   gaining a `group` field is the remaining piece. A consumer treats
   **each group as its own ordered substream**; cross-group interleave on the wire
   is *by design* (different priority/cadence — there is no global order to keep).
   `run_id` may stay globally unique, but "in order" is a per-group guarantee.
3. **Consumer caveat (HMI):** one script can emit the **same var name** under
   different groups (e.g. the same `verdict`), so a naive consumer would let `low`
   clobber `high`. Key results by **`(group, name)`** (or branch the script so var
   names differ per source). The HMI should namespace its `state.vars` by group.

There is no "global single output order" anymore — only per-group order plus an
intentional cross-group interleave the consumer demultiplexes by `group`.

## Implementation sketch

- `TriggerEvent` (`xi_trigger_bus.hpp`) += `std::string group`. Source sets it from
  its instance config; the bus stamps `default_group` if empty.
- Replace the single `g_ev_queue` with **one queue per group** + a per-group
  running counter. `enqueue_event_` routes by group and applies that group's
  `queue_depth`/`overflow`/`min_interval_ms`.
- Each group spawns its own `max_parallel` workers; **each worker is pinned to its
  group's queue** and its OS priority is set once at spawn from `thread_priority`.
  No shared pool, no cross-group pick.
- Result ordering is **per-group**: each group keeps its own emit-sequence (the
  current single `g_dispatch_seq`/`g_result_ordered` become per-group state), and
  `vars`/`run_started`/`run_finished` carry a `group` field so consumers can
  demultiplex. Legacy single-group keeps today's exact behaviour.
- `PluginManager`/project parsing: read `parallelism.groups` + `default_group`;
  when absent, synthesize one implicit group from legacy
  `dispatch_threads`/`queue_depth`/`overflow` (single code path).
- Surface per-group depth/running in `dispatch_stats`.
- `// TODO(linux):` thread priority via `pthread_setschedparam` / `nice`;
  `SetThreadPriority` is the Windows path (gate `#ifdef _WIN32`).

## Increment plan

- **v1 — shipped.** `TriggerEvent.group`; one queue + own `max_parallel` workers
  per group at its `thread_priority` (Win32 `SetThreadPriority`, `TODO(linux)`);
  `parallelism.groups` + `default_group` parsing with legacy single-group fallback;
  `instance.json` `"group"` routing (by the emitting source's `leader_source`);
  per-group `dispatch_stats`; gated so no-groups projects are byte-identical to
  before. Smoke + gating test: `examples/qa_dispatch_groups/`.
- **v1 hardening (post multi-agent review).** Fixed: lane-lifetime UAF + the
  overflow:block wait UAF (lanes are now `shared_ptr` guarded by `g_lanes_mu`;
  producers hold a ref and re-check `g_continuous` after taking the lane lock);
  image-handle leak on stop (lanes are drained before destruction, mirroring the
  legacy `g_ev_queue` drain); `max_parallel` clamped to [1,32] + `queue_depth` to
  [1,10000]; `lane_for_` unknown-group fallback goes to `default_group` (not
  blindly the first lane); parse-time warnings for duplicate group names (skipped),
  unknown `thread_priority`/`overflow`, and a `default_group` naming no real group.
  Covered by `qa_dispatch_groups` tests B (clamp) + D (warnings).
- **v1.1 — per-group `result_order`** ✅ shipped. Each `GroupLane` owns an
  `EmitGate`; `result_order: "arrival"` (+ `max_parallel>1`) serialises that group's
  emission by frame-arrival order, independent of other groups. The legacy single
  pool keeps its one global gate. Regression: `qa_dispatch_groups` Test E.
- **Two-path routing regression** ✅ shipped — `examples/qa_two_group_paths/` runs
  two `frame_source` instances tagged (via `instance.json "group"`) into different
  lanes (`src_fast`→fast 40fps/3thr, `src_slow`→slow 8fps/1thr) and proves the
  dispatcher routes each by the *emitting* instance's group: zero cross-routing
  (each lane only ever carries its own source) and each path runs at its own cadence
  (~119 vs ~24 results / 3s). This is the self-emitting-source case the
  load-separation test needed.
- **Per-group max_parallel + ordering regression** ✅ shipped —
  `examples/qa_group_parallelism/` uses a `burst_source` per group that fires 3× the
  group's worker count every second (instantaneous surge); each inspect sleeps a
  random 50-100ms so work overlaps. Polling `dispatch_stats.running` shows each
  group's peak in-flight reaches *exactly* its `max_parallel` (p1→1, p2→2, p4→4) and
  never exceeds it, with throughput scaling 1:2:4. The groups are also
  `result_order:"arrival"`, and the driver asserts **0 run_id inversions per group**
  — so parallelism and ordered emission hold *together* under the same bursty load
  (4 concurrent out-of-order completions still emit in arrival order).
- **Per-group `cpu_affinity`** ✅ shipped — multi-core mask per group (default
  unbound). `examples/qa_cpu_affinity/` verifies via `GetCurrentProcessorNumber`
  that a `[2,3,4,5]`-bound group only ever ran on cores 2-5 (and used >1 of them —
  a real mask, not a pin), while the unbound group spread across other cores.
- **`min_interval_ms` rate limit** ✅ shipped — a group caps its dispatch rate to
  one start per `min_interval_ms` (workers CAS-claim a slot ≥ that spacing and sleep
  to it; surplus coalesces via `drop_oldest`). `examples/qa_min_interval/`: a steady
  20/s into a `min_interval_ms:100` group is processed at ~10/s (cap), dropping the
  surplus, while an uncapped group runs full-rate.
- **Deferred follow-ups.** The `group` wire tag on `vars`/`run_started`/`run_finished`
  (only `run_result` carries `group` today); the
  *latency* half of load-separation (assert a saturated `low` doesn't raise `high`'s
  p99 — qa_two_group_paths proves routing/cadence, not yet a latency bound).
- **Later (only if ever needed)** — a shared-pool / oversubscribed mode with a real
  priority queue. Out of scope: group-owned threads already meet the goal and
  threads are cheap, so there's no shared pool to schedule.

## Tests

- A `low` group running long inspects at its `max_parallel` does **not** raise
  `high`-group latency (the headline guarantee) — measure high's p99 with low idle
  vs saturated.
- `min_interval_ms` caps a group's dispatch rate; surplus coalesces to latest.
- **Per-group ordering**: with each group in `arrival` mode, each group's `vars`
  stream is in-order *within the group* even when another group lags; the `group`
  tag lets a consumer separate the substreams; a saturated `low` group does not
  perturb `high`'s emission order.
- Untagged triggers land in `default_group`; legacy projects (no `groups`) behave
  identically to today (global ordering preserved for the single implicit group).
- Cross-platform: thread-priority set is gated; the rest is portable.

## See also

- `docs/guides/writing-a-script.md` → Parallel dispatch (`dispatch_threads`,
  `queue_depth`, `overflow`, `result_order`) — the current single-pool model.
- `docs/guides/adding-a-plugin.md` — the `reentrant` flag (matters for the shared-
  instance priority-inversion caveat).
