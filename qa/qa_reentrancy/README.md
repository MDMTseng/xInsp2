# qa_reentrancy — declared reentrancy + per-instance concurrency cap

Proves the safety + sizing model for the parallel dispatch pool
(`parallelism.dispatch_threads > 1`). The `concurrency_probe` plugin reports the
max number of dispatch workers seen inside its own `process()` at once.

`driver.py` runs one continuous session at `dispatch_threads=4` and pokes three
instances every frame:

| Instance | Plugin | Declares | Expected max concurrency |
|---|---|---|---|
| `serial`   | `concurrency_probe`    | (not reentrant)            | **1** — host serializes per instance |
| `parallel` | `concurrency_probe_rt` | `reentrant: true`          | **≥ 2** — runs concurrently |
| `capped`   | `concurrency_probe_rt` | `reentrant` + instance.json `max_concurrency: 1` | **1** — per-instance cap holds |

So a plugin is **safe by default** (unflagged → serialized), opts into
parallelism with `reentrant: true`, and a reentrant instance can be **sized** with
`max_concurrency` (the non-reentrant lock is just the M=1 case of that cap).

```
python driver.py     # VERDICT: PASS
```

Windows-only (plugin compile); skips on non-`nt`. See
`docs/guides/write-a-script.md` (Parallel dispatch) and
`docs/reference/instances.md` (`max_concurrency`).
