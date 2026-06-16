# process_overhead — isolation:in_process vs isolation:process

Measures the overhead of running a single source plugin under
`isolation:"process"` (separate `xinsp-worker.exe`) vs
`isolation:"in_process"` (same address space as the backend).

Reuses the `burst_source` plugin from `cross_proc_trigger`. Each
mode gets its own backend process so RSS deltas are clean.

## How to run

```
python driver.py
```

Writes `results.json` next to the script.

## One run, 60 Hz steady source, 3 s window (2026-05-09, Win11, Release backend)

| metric                | thread      | process     | Δ            |
|-----------------------|-------------|-------------|--------------|
| open_project          | 2968.0 ms   | 3719.0 ms   | **+751 ms**  |
| first-trigger latency | 0.0 ms*     | 15.0 ms     | +15 ms       |
| backend RSS           | 13.75 MB    | 14.17 MB    | +0.42 MB     |
| worker RSS (sum)      | 0.00 MB     | 23.07 MB    | **+23.07 MB**|
| events / 3 s          | 174         | 175         | +1           |
| sustained rate        | 58.0 Hz     | 58.3 Hz     | +0.3 Hz      |
| gap mean              | 17.34 ms    | 17.24 ms    | -0.10 ms     |
| gap p95               | 31.00 ms    | 31.00 ms    | 0.00 ms      |
| gap stdev             | 5.75 ms     | 6.08 ms     | +0.33 ms     |

\* The thread-mode "0 ms" is misleading: `BurstSource`'s constructor
spawns its run loop, so a VAR was already in the queue from before
`cmd:start`. The 15 ms in process-mode is the real first IPC roundtrip.

## What this means

Per-process overhead breaks down as:

- **One-time on `open_project`**: ~750 ms wall (worker spawn +
  named-pipe handshake + `RPC_CREATE`). Multiplied by N
  process-isolated instances at project open.
- **Resident memory**: ~23 MB working set per worker. With a
  10-instance project all process-isolated, expect ~230 MB extra RSS.
- **Steady-state per-event**: effectively zero. At 60 Hz steady,
  rate / mean gap / p95 / stdev all sit within noise of the
  thread-mode baseline. IPC goes through named pipes + shared memory
  (see `docs/archive/ipc-shm.md`) which costs single-digit µs per
  hop.

So the cost model is **per-instance, not per-event**: process
isolation is cheap to keep running and only expensive to stand up.
