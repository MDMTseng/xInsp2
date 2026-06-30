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
source** (`burst_pipeline`'s `frame_source`) several times; survival with no minidump is
the signal.

## Fixtures

Uses sibling example projects rather than its own: `../qa_sink_shared_doc` (2 plugin
instances) for `#7`, `../burst_pipeline` (a self-emitting source) for `#6`.

```
python driver.py
# == #2 bind-fail clean exit ==           PASS
# == #7 clean shutdown with project ==    PASS
# == #6 open/close cycle under source load == PASS
# VERDICT: PASS
```
