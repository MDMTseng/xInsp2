# qa_recover — FE supervisor recover-and-clear (Phase G, #92)

`fe_supervisor` proves the FE's **give-up** path (a backend that crashes forever
→ safe-state → respawn → hit the cap → stay safe). This example proves the
**happy exit** that path can't: a backend that crashes a *few* times and then
**recovers** must have its safe state **cleared**, and the FE must **not** hit
the respawn cap (test plan FE-E5 / safety property SP4).

The `crash_then_heal` plugin crashes the backend its first `crash_count` (2)
starts — uncatchably, via a raw `std::thread` null-deref, exactly like
`raw_thread_crash` — counting in a marker file that survives the process dying
(`XI_QA_RECOVER_MARKER`, inherited FE → backend → plugin). Once the threshold is
reached it returns normally forever, so the backend stays up.

`driver.py` launches `xinsp-fe.exe` on this project and asserts:

- ≥1 `ENTER SAFE STATE reason=BackendExit` (death detected, line driven safe),
- ≥1 `respawning backend`,
- `CLEAR SAFE STATE` present — **the recover transition, the whole point**,
- **no** `RespawnLimitExceeded` (recovered before the cap),
- the marker counted **exactly** `crash_count` crashes,
- the backend is up and the FE still running at the end; no orphan after shutdown.

```
python driver.py     # VERDICT: PASS
```

Windows-only today (process spawn / Job Object). Skips on non-`nt`. See
[`docs/archive/fe-be-split-test-plan.md`](../../docs/archive/fe-be-split-test-plan.md)
"Phase G".
