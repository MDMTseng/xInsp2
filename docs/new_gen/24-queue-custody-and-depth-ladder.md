# 24 — Queue custody vs policy, and the depth ladder

Status: **design record + landed depth-0 rendezvous** (2026-07-04). Grew out of
the design dialogue behind doc 19 D1 (mux/priority sink) and the `overflow:block`
reintroduction (doc: commit ee65bc7). Answers a recurring question — *"can the
dispatch queue be a plugin's job?"* — and records the `queue_depth` semantics
including the new `0` (rendezvous) rung.

## 1. Custody vs policy — the seam

A dispatch lane's queue does several things. They split cleanly into what MUST
stay in the core and what is a legitimate plugin seam:

| The queue does… | Kind | Home |
|---|---|---|
| buffer trigger events while workers are busy | mechanism | **core** |
| own event lifetime (each queued event pins ImagePool + meta refs; drop → `release_trigger_event_`) | mechanism, **memory-safety-critical** | **core** |
| claim the arrival/emit seq (`arrival_id` at push, gate seq at dequeue) → arrival==emit order | mechanism, coupled to the emit gate | **core** |
| participate in quiesce/teardown (drain-before-DLL-op) | mechanism, lifecycle-coupled | **core** |
| decide *which* event to admit / drop / reorder under pressure | **policy** | **pluggable** |

The rule: **custody stays in the core; only policy is a plugin seam.** Moving
custody across the ABI would drag the ref-lifetime ledger (the R1 UAF surface),
the quiesce coupling, and the seq-claim across the boundary — enormous blast
radius on exactly the safety-critical machinery.

And the *right* place for the policy seam is the **output side** — a
compute-order → wire-order reorder/priority sink (doc 19 D1), which is a normal
data-plane plugin needing **zero core change** (U3 ordered-sink contract, doc 17
already makes it expressible). NOT the pre-compute admission path: that is hot
(per-event, under the lane lock), touches ref custody, and its policy space is
tiny (drop_oldest/drop_newest/block). A per-event admission capability would be
the P2 head-of-line anti-pattern one layer up.

## 2. "Plugin owns the queue" — how to get it TODAY, no core rewrite

You don't move the queue into a plugin; you shrink the **core** buffer so the
plugin's own buffer becomes the only real queue, and let the plugin pace itself:

- **Source plugin holds its own deep queue** and emits into a shallow core lane.
- Core keeps a **deadlock-free minimal residual** (the depth rung you pick) plus
  its drop/back-pressure policy as a safety valve the plugin can't stall.
- The plugin **paces emits off completion feedback** (`run_result`) or the
  back-pressure of `overflow:block` — there is no separate push-back signal.

Worked exemplar: **`examples/qa_plugin_queue_sim/`** (LANDED) — a `queue_source`
plugin holding its own deep `std::deque` (backlog 500) and pacing its emits purely
off the core lane's back-pressure, over a `depth=0` (rendezvous, group `rv`) and a
`depth=1` (pipeline, group `pipe`) lane. Measured: both lossless (emitted==processed,
0 drops), core lane `high_watermark == 1` on BOTH (the plugin's own deque holds the
backlog — the core never buffers past the single handoff), the rendezvous is strict
lock-step (2nd emit returns only after the 1st frame is TAKEN → `first_gap_ms ≈` the
inspect time, `fast_returns == 0`) while the pipeline runs one ahead (`first_gap_ms
≈ 0`, `fast_returns == 1`), and stop+close_project while the source is parked in the
rendezvous returns in ~0.06s.

## 3. The depth ladder

`queue_depth` (per group; project default 100; range now **[0, 10000]**) is the
size of the core's residual buffer. The rungs are qualitatively different:

| depth | name | producer/worker coupling | core buffer | use |
|---:|---|---|---|---|
| **0** | **rendezvous** | strict lock-step: producer blocks until a worker **takes** the event | zero | plugin owns 100% of the queue; cleanest ownership boundary |
| **1** | 1-deep pipeline | producer may run **one** ahead (prepare next while worker computes current) | one in-flight-adjacent slot | overlap producer prep with worker compute; near-passthrough |
| **N** | buffer | N slots of decoupling | N | absorb bursts; classic |

The 0↔1 distinction is precise: **whether the producer waits for its event to be
TAKEN before the enqueue returns.** depth=1 returns as soon as there is *space*
(so the producer runs one ahead); depth=0 returns only once a worker has *taken*
the event (no run-ahead, zero core limbo).

### depth=0 rules (landed)

- **Requires `overflow:"block"`.** With a drop policy, depth=0 has no slot to hold
  the event and no rendezvous wait, so it degenerates to "drop unless a worker is
  idle this instant" (≈ drops everything) AND would hit `front()` on an empty
  queue. The parser therefore **rejects depth=0 unless overflow==block** and warns
  + clamps to 1 otherwise.
- **Rendezvous mechanism:** the producer waits for the handoff slot to be free,
  deposits, then waits (on a per-take **generation counter**, `GroupLane::
  taken_count`, bumped by the worker at dequeue) until *its* event is taken — not
  merely until the slot is empty, so interleaved producers can't confuse another's
  handoff for their own.
- **Interruptible:** a stop/teardown wakes the rendezvous (predicate keys off
  `g_eng.continuous`); the producer degrades to a drop, never hangs teardown —
  same discipline as `overflow:block` (cv_not_full stop-wake).
- **max_parallel:** intended for `max_parallel:1` (true lock-step). With N>1 it
  still functions but serializes to one in-flight event (each producer waits for
  its handoff before the next), underusing the extra workers — documented, not
  forbidden.
- **Same DANGER as block:** only for a back-pressure-TOLERANT source on a
  dedicated thread. NEVER a lane a worker can self-feed, or a real-time camera
  callback. Config-side responsibility.

Landed as: `service_internal.hpp` (`GroupLane::taken_count`), `service_dispatch.cpp`
(worker bumps `taken_count` under the lane lock at dequeue; `enqueue_to_lane_` gains
the `depth==0` rendezvous branch that runs BEFORE the block/push/drop logic and
returns before any drop path), `xi_pm_project.hpp` (queue_depth clamp min 0 + the
"depth 0 requires overflow:block, else warn+clamp to 1" validation at both project
and group scope), `xi_project_model.hpp` (field docs). Test: `examples/
qa_plugin_queue_sim/` (run_qa `qa_*` glob; strict PASS x2).

## 4. What is explicitly NOT built

- No admission-side dispatch-policy capability (custody must stay core; the
  output-side D1 sink is the sanctioned policy seam).
- No moving event storage / ref custody / seq-claim into a plugin.
