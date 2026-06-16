# qa_result_order — arrival-ordered results under a parallel pool

Under `parallelism.dispatch_threads > 1`, per-frame results (`vars` + previews +
`run_finished`) are emitted as each worker **finishes** by default
(`result_order: "completion"`) — so with uneven inspect times the wire stream is
out of frame order. `result_order: "arrival"` makes the backend replay results
in **frame-arrival order**: a worker that finishes early waits its turn before
emitting. Compute still runs fully parallel; only emission is gated. (`run_id` is
assigned at dequeue, so it tracks arrival order and is monotonic on the wire in
arrival mode.)

`driver.py` runs the same uneven-timing script (every 5th frame slow,
`dispatch_threads=4`) under both modes and records each `vars` message's `run_id`
in receive order:

- **completion** → reorders (inversions > 0), proving the workload really does
  finish out of order under this pool;
- **arrival** → zero inversions on that same workload.

```
python driver.py     # VERDICT: PASS
```

Windows-only (plugin/script compile); skips on non-`nt`. See
`docs/guides/write-a-script.md` (Parallel dispatch → `result_order`).
