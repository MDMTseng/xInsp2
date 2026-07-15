# qa_lifecycle_teardown — shutdown / lifecycle UAF regression

Guards three host teardown/lifecycle paths found in the 2026-06-30 core bug-hunt
(`docs/roadmap/core-bug-hunt-2026-06.md`). Each asserts the process exits/continues
**cleanly** and writes **no spurious minidump** to `%TEMP%/xinsp2/crashdumps`.

| Check | Bug | Before the fix | Fix |
|-------|-----|----------------|-----|
| `#2` bind-fail | watchdog `std::thread` still joinable on the bind-fail `return 1` | `std::terminate` at static destruction → crash filter fabricates a minidump on a routine port-in-use | `WatchdogJoiner` RAII stops+joins the watchdog on every early return |
| `#7` clean shutdown | no normal exit path called `close_project` | `~PluginManager` destroys instances AFTER `FreeLibrary` (destroy_fn into unmapped code) + against a destroyed `ImagePool` singleton → AV on graceful shutdown | `controlled_shutdown_teardown_` calls `close_project`; `~PluginManager` clears instances before `FreeLibrary`; adapter image-sweep guarded by `g_image_pool_alive` |
| `#6` open/close under load | bus sink stayed installed across the plugin `FreeLibrary` | a source emitting on its own thread launches a one-shot inspect into freed code | `quiesce_dispatch_for_lifecycle_op_` pauses detached launches + clears the sink + drains before the unload (reversed by the guard for resuming ops) |

The `#6` race is non-deterministic, so the test cycles open/close **under a free-running
source** several times; survival with no minidump is the signal.

## Fixtures

Self-contained under `./fixtures/burst_proj`: two `burst_source` instances (a source
that emits on its own thread) driving a trivial inspect. One fixture supplies both
properties the checks need — live plugin instances for `#7`, and a self-emitting source
for the `#6` close-mid-stream race.

Earlier revisions borrowed sibling example projects (`../qa_sink_shared_doc` for `#7`,
`../burst_pipeline` for `#6`); both were removed by THE CUT (commit `0042385`), which
silently broke this test (`#7` failed at `open_project` with "failed to open project").
Owning the fixture makes the test independent of sibling churn.

## Load sensitivity (runs serial)

Listed in `examples/qa_serial.txt` so it runs alone: the `#6` open/close cycle recompiles
the inspect each cycle (ccache-cached, so cheap) and its clean-teardown timing loses under
CPU oversubscription. Running serially, off the parallel pool, keeps it deterministic.

```
python driver.py
# == #2 bind-fail clean exit ==           PASS
# == #7 clean shutdown with project ==    PASS
# == #6 open/close cycle under source load == PASS
# VERDICT: PASS
```
