# qa_watchdog — per-worker watchdog + exit-on-hard-trip

The inspect watchdog (`--watchdog=MS`) used to track a **single** deadline slot
and was **disabled** when `parallelism.dispatch_threads > 1` — a wedged worker
under a parallel pool went undetected. Two changes (burst-parallelism item #1):

1. **Per-worker:** the watchdog now tracks a deadline **slot per in-flight
   inspect**, so it protects every dispatch worker under N > 1.
2. **Hard trip exits for respawn:** if a script ignores the cooperative-cancel
   grace, the backend **exits** with `WATCHDOG_EXIT_CODE` (`0x5744`) so the FE
   supervisor respawns a clean one. It no longer `TerminateThread`s a worker —
   that would leak the per-instance lock (deadlocking the instance) and risk
   heap corruption.

`driver.py` runs the backend directly with `dispatch_threads=4` and a runaway
inspect (busy loop, never polls cancel), and asserts the backend exits on its
own with `0x5744` and logs `watchdog HARD trip`.

```
python driver.py     # VERDICT: PASS
```

The equivalent N=1 path (and the WS-level contract) is covered by the extension
test `vscode-extension/test/runWatchdog.mjs`. Windows-only (plugin/script
compile); skips on non-`nt`. See `docs/guides/write-a-script.md` (Parallel
dispatch).
