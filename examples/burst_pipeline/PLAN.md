# burst_pipeline — PLAN

## Goal

A realistic two-plugin pipeline (heavy source + light classifier) used
to *observe* (not just verify) how `parallelism.dispatch_threads` and
`parallelism.queue_depth` interact under live load. Three sweeps:

| Sweep | dispatch_threads | queue_depth | What I expect to learn |
|---|---|---|---|
| 1 | 1 | 32 | Serial baseline. Throughput ceiling = 1 / (heavy + light) per inspect. |
| 2 | 4 | 32 | Real speedup vs ideal 4x. Where does it fall short? |
| 3 | 4 | 4 | Drop policy bites. drop_oldest should keep queue bounded; latency should drop. |

Heavy source `process()` is unused on the dispatch hot path — the source
only `emit_trigger`s. The "heavy ~30 ms" sleep belongs in the **inspect
script**, otherwise dispatch_threads has nothing to parallelise. (The
docs talk about this — N inspect threads run `xi_inspect_entry`
concurrently. Source emits are decoupled.)

So the real layout:
- `frame_source` plugin: ticks at fps Hz, emits a 320x240 grayscale
  frame with the seq stamped in first 8 bytes. NO sleep here — it's a
  real-rate source.
- `light_classifier` plugin: `process()` reads the seq from the input
  image, sleeps 5 ms, returns `{seq, kind}`.
- `inspect.cpp`: pulls `current_trigger().image("source0")`, sleeps
  ~30 ms (the "heavy CV work" simulation), then calls
  `xi::use("classifier").process(...)`, then VARs.
  Total per-inspect cost ~35 ms.

Predictions:
- **N=1**: ceiling ~ 1000/35 = ~28 events/sec. With fps=60 source,
  queue saturates, drops kick in.
- **N=4**: ceiling ~ 4000/35 = ~114 events/sec, but source caps at 60.
  Should land near 60 events/sec with no drops, ~latency = inspect_ms.
- **N=4, queue=4**: same throughput (60), but with the queue tight,
  drop_oldest should activate occasionally if classifier reentrancy
  or scheduler jitter pushes queue past 4. Per-event latency should
  be tighter (newer frames preferred).

## Open questions / things I expect to bite

1. **Plugin reentrancy.** `light_classifier` will be called by 4
   inspect threads at once. Need it to be reentrant — use atomics, no
   shared mutable state. Manifest `thread_safe: true` is documented as
   "no current effect" so this is purely on me.
2. **Source isolation.** Doc says source plugins must be `in_process`
   because cross-process emit_trigger is not fully wired. Will mark
   accordingly in `instance.json`.
3. **TID collisions.** The source emits with `tid.lo = seq + 1`. Under
   `parallelism > 1` the bus still dispatches one inspect per
   trigger; that should be fine since policy is single-source (Any
   policy effectively).
4. **VAR redeclaration trap.** In inspect, I'll need to avoid using
   `seq` and `kind` as names that clash with locals from the
   classifier result. Will use `kind_v` etc.
5. **What does `dispatch_stats` actually return mid-flight?** Doc
   shows it; I'll snapshot before/after to see deltas. Might need to
   poll while running for queue_depth_max.
6. **Latency measurement.** `t.timestamp_us()` is the source emit
   timestamp; `now() - that` is end-to-end latency including queue
   wait. That's the metric I want.

## Work order

1. Write `plugin.cpp` + `plugin.json` for `frame_source` (modeled on
   `synced_cam` but simpler: one source, single output, ~60 Hz with
   `sleep_until` to avoid drift).
2. Write `plugin.cpp` + `plugin.json` for `light_classifier`.
3. Write `inspect.cpp` reading current_trigger, simulating 30 ms heavy
   work, calling classifier.process, VARing seq/kind/latency_us.
4. Write `project.json` with parallelism block + trigger_policy=Any.
5. Write `instances/source0/instance.json` and `instances/classifier/instance.json`.
6. Write `driver.py` doing the three sweeps. Write parallelism config
   into `project.json` between sweeps using a small file-rewrite
   helper, then re-`open_project`. This is the only knob `cmd:start`
   doesn't take.
7. Run, capture results, write RESULTS.md.

## Open question I genuinely don't know the answer to

Can `parallelism.dispatch_threads` be changed by editing project.json
mid-session and re-`open_project`-ing, or do I need to teardown the
backend between sweeps? Will try the cheap path first.
