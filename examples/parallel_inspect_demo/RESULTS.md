# parallel_inspect_demo

Answers the practical question: **if `process()` takes a long time
AND multiple triggers arrive close together (e.g. hardware-
synchronised cameras), can the inspect calls run in parallel?**

Short answer: yes — `parallelism.dispatch_threads` in `project.json`
controls how many dispatcher worker threads pull events off the
trigger queue. With `dispatch_threads=N`, up to N inspects run
concurrently. Already shipped in PR #20.

This example exists to demonstrate the effect on the exact case the
question asks about: 3 simultaneous camera triggers + a 100 ms
inspect.

## Topology

- 3 `burst_source` instances at 30 fps steady — total **90 events/s
  offered** to the dispatcher.
- `inspect.cpp` does `sleep_for(100 ms)` — stand-in for a real CV
  pipeline (template match, deep model, etc).

## Run

```
python driver.py
```

Driver runs the project twice — once with `dispatch_threads=1`
(serial), once with `dispatch_threads=3` (parallel) — and reports
inspect rate over a 4 s window.

## Headline numbers (Win11, Release backend, 2026-05-10)

| metric                | serial (N=1) | parallel (N=3) |
|-----------------------|--------------|----------------|
| active inspects (4 s) | 37           | 111            |
| rate                  | 8.9 /s       | 26.5 /s        |
| inspect_us mean       | 107.5 ms     | 107.5 ms       |
| inspect_us p95        | 109.6 ms     | 109.7 ms       |

**Speedup: 2.98×** (theoretical max with 3 threads: 3.00×).

`inspect_us` is identical across modes — each inspect still takes
~100 ms; they just run on parallel threads in the N=3 case.

## What this confirms

- `dispatch_threads` does what it says — for CPU-bound (or sleep-
  bound) inspect work, scaling is near-linear up to `min(N_cores,
  events_per_window)`.
- Per-source ordering is *not* enforced by the dispatcher pool. Each
  source's events round-robin onto whichever worker pulls them first.
  If your pipeline needs strict per-camera ordering, set
  `dispatch_threads=1` (or partition triggers via `policy:
  "leader_followers"` / `"all_required"`).
- Backpressure: the offered rate (90/s) exceeds the parallel
  capacity (~30/s). The excess gets handled by the
  `parallelism.queue_depth` / `overflow` policy (here:
  `drop_oldest`, queue_depth=32). To avoid drops, raise
  `dispatch_threads` (more parallel inspects) or shrink the per-
  inspect work.

## When to raise dispatch_threads

Rule of thumb: set N to roughly the number of physical cores
available, capped at the number of source instances if every source
fires at near-line-rate. More threads than cores leads to context-
switch thrash; more threads than offered events is wasted.
