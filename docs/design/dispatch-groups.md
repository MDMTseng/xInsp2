# Dispatch groups — priority + concurrency for trigger work (design)

> **Status: design, not scheduled.** Captures the agreed model so it can be built
> in increments. Assessment date 2026-06-04.

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
by its own `max_parallel`). **Groups are then decoupled at the dispatch layer** —
no group ever waits for another's slot. What remains is pure **compute-resource
occupancy**, and the OS handles that:

- **OS thread priority does real preemption on the CPU.** Run the low-priority
  group's workers at a lower OS thread priority; when cores are scarce the OS
  preempts low-priority threads for high-priority ones (Windows + Linux are
  priority-preemptive). We *can't* preempt a running inspect off its worker slot,
  but with enough slots we don't need to — and the OS preempts at the core level.
- **`max_parallel` bounds each group's footprint** so a bursty best-effort group
  can't spawn 100 concurrent inspects and thrash memory/cores.

So in the decoupled regime the "scheduler" is trivial: each group has its own
slots + FIFO; there is **no cross-group priority queue**.

### Residual coupling (the decoupling is not total)

"Only compute occupancy" holds **only if groups use disjoint resources**. Still
couples them:

1. **A shared non-reentrant plugin instance** — its lock serializes callers, and a
   low-pri inspect holding it blocks a high-pri one = **priority inversion**.
   Mitigation: don't share an instance across priority groups (or mark it
   `reentrant`). We can't do RTOS priority-inheritance on a plain mutex easily.
2. **Shared ImagePool / memory bandwidth / disk-I/O** — a heavy group can pressure
   these; thread priority doesn't arbitrate memory bandwidth.

## Two modes

- **Decoupled (default, v1).** `dispatch_threads ≥ Σ max_parallel`. Per-group
  `max_parallel` + `thread_priority` (+ optional rate/queue). No priority queue.
  Satisfies the "critical must stay fast, best-effort may lag" need.
- **Oversubscribed (advanced, opt-in / v2).** `dispatch_threads < Σ max_parallel`
  to cap total threads — groups now compete for a smaller shared pool, so we add a
  real **priority queue** (a freed worker takes the highest-priority group that has
  queued work and is under its cap) + worker reservation + anti-starvation
  (`weight` / aging). Only needed when you deliberately under-provision threads.

## Group parameters

| Param | Mode | Meaning |
|---|---|---|
| `name` | both | group id; triggers carry it |
| `max_parallel` | both | max concurrent inspects for this group (footprint cap; in decoupled mode, its slot reservation) |
| `thread_priority` | both | OS thread priority of this group's workers: `high` / `normal` / `low` (→ `THREAD_PRIORITY_ABOVE_NORMAL` / `NORMAL` / `BELOW_NORMAL`) |
| `queue_depth` + `overflow` | both | per-group buffering (`drop_oldest` for live, `block`/deep for archival) |
| `min_interval_ms` | both | rate limit: dispatch this group at most once per N ms; surplus triggers coalesce → keep latest (the *deliberate* way to "reduce frequency", vs sleeping a worker) |
| `priority` | oversubscribed | integer; which group a freed shared slot goes to first |
| `weight` / `max_age_ms` | oversubscribed (v2) | anti-starvation share; drop a trigger queued longer than N ms |

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
  "dispatch_threads": 5,            // == 4 + 1, so both groups are fully decoupled
  "default_group": "high",
  "groups": [
    { "name": "high", "max_parallel": 4, "thread_priority": "normal",
      "queue_depth": 8,  "overflow": "drop_oldest" },
    { "name": "low",  "max_parallel": 1, "thread_priority": "low",
      "queue_depth": 50, "overflow": "drop_oldest" }
  ]
}
```

- **high**: up to 4 concurrent inspects, normal OS priority — the critical path.
- **low**: 1 concurrent inspect, below-normal OS priority — best-effort; can lag,
  yields the CPU to `high` when cores are scarce, never steals high's slots.
- `dispatch_threads` 5 = 4 + 1 → decoupled (no priority queue needed).

**Backward compatibility:** if `parallelism.groups` is absent, the dispatcher
stays exactly as today — one implicit group sized by `dispatch_threads`. Grouping
is opt-in; existing projects are unaffected.

## Assigning a trigger to a group

A `TriggerEvent` gains a `group` field. The **emitting source instance declares
it** in its `instance.json`:

```json
{ "plugin": "mock_camera", "group": "high" }
```

The backend resolves source→group on `emit_trigger` (untagged → `default_group`).
Chosen over a project-level source→group map or an `emit_trigger` arg because the
source is the natural owner of "how urgent is my stream".

## Implementation sketch

- `TriggerEvent` (`xi_trigger_bus.hpp`) += `std::string group`. Source sets it from
  its instance config; the bus stamps `default_group` if empty.
- Replace the single `g_ev_queue` with **per-group queues** + per-group running
  counter + `max_parallel`. `enqueue_event_` routes by group and applies that
  group's `queue_depth`/`overflow`/`min_interval_ms`.
- Worker loop: pop from the worker's own group (decoupled mode each worker is
  pinned to a group sized to its `max_parallel`); set the thread's OS priority once
  at spawn from `thread_priority`. (Oversubscribed mode: a shared worker picks the
  highest-`priority` eligible group — that path is v2.)
- `PluginManager`/project parsing: read `parallelism.groups` + `default_group`;
  default to the single-group legacy path when absent.
- Surface per-group depth/running in `dispatch_stats`.
- `// TODO(linux):` thread priority via `pthread_setschedparam` / `nice`;
  `SetThreadPriority` is the Windows path (gate `#ifdef _WIN32`).

## Increment plan

- **v1** — `TriggerEvent.group`, per-group queues + `max_parallel` + worker OS
  `thread_priority`, config parsing + the default 2-group set, `instance.json`
  `"group"`, `dispatch_stats` per group, regression test (`examples/qa_dispatch_groups/`:
  prove a saturated `low` group never delays `high`). Decoupled mode only.
- **v2** — oversubscribed priority queue + reservation + `weight`/aging
  anti-starvation + `max_age_ms`.

## Tests

- A `low` group running long inspects at its `max_parallel` does **not** raise
  `high`-group latency (the headline guarantee) — measure high's p99 with low idle
  vs saturated.
- `min_interval_ms` caps a group's dispatch rate; surplus coalesces to latest.
- Untagged triggers land in `default_group`; legacy projects (no `groups`) behave
  identically to today.
- Cross-platform: thread-priority set is gated; the rest is portable.

## See also

- `docs/guides/writing-a-script.md` → Parallel dispatch (`dispatch_threads`,
  `queue_depth`, `overflow`, `result_order`) — the current single-pool model.
- `docs/guides/adding-a-plugin.md` — the `reentrant` flag (matters for the shared-
  instance priority-inversion caveat).
