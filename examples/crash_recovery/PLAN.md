# crash_recovery — PLAN

## Goal
Verify that xInsp2 absorbs hard crashes inside a plugin's `process()` without
taking the backend down, and that subsequent calls (including a live
`exchange_instance` to bump a config knob) keep working.

## Approach

### Plugin: `count_or_crash`
- Single C++ plugin under `plugins/count_or_crash/`.
- Inherits `xi::Plugin`. Reads input image `src` (1-channel grayscale). If
  the image is RGB, falls back to `cvtColor(RGB2GRAY)` defensively, but the
  driver will feed grayscale.
- Threshold at 127 -> mask. Run `cv::connectedComponents`. `count` = labels-1.
- If `count > crash_when_count_above` (default 8), do a hard null-deref:
  `*(volatile int*)nullptr = 42;`. This is an access violation on Windows,
  not a C++ exception, so it exercises real crash isolation, not the
  `_set_se_translator` shim documented in adding-a-plugin.md (and definitely
  not throw / catch).
- Otherwise return `{"count": int, "mask": image}`.
- `exchange()` with `{"command":"set_threshold","value":N}` updates
  `crash_when_count_above` and returns post-state via `get_def()`.
  Also support `{"command":"reset"}`.
- `get_def`/`set_def` persist `crash_when_count_above` only.
- `plugin.json` lists the manifest with the one tunable.

### Instance: `cnt`
- `instances/cnt/instance.json` configures the plugin with the default
  `crash_when_count_above = 8`.
- Use the **default** isolation (so `instance.json` deliberately does NOT
  set `"isolation"`). Per `docs/reference/instance-model.md` the default is
  `"process"`, which is exactly what we're trying to validate. If the docs
  are right, the worker process eats the segfault and the backend survives;
  if it's wrong, the backend dies and we have a P0.

### Test data
- 10 frames, 320x240, grayscale PNG.
- "Happy" frames (≤ 5 blobs): seeded RNG, draw 4-5 dark filled circles on a
  noisy gradient bg. With threshold 127 and noise of σ ≈ 8 the bg stays
  bright, blobs come through black, gives ~5 connected components.
- "Crash" frames (≥ 12 blobs): seeded RNG, draw ~12 dark blobs. count > 8,
  triggers the segfault.
- Mix order: `H H C H C H H C H H` (3 crash, 7 happy). Saves
  `ground_truth.json` with `[{file, blob_count}, ...]`.

### Driver
1. `c.open_project(crash_recovery)` — backend scans plugins/, compiles
   `count_or_crash`, instantiates `cnt` in default (process) isolation.
2. `c.compile_and_load(inspect.cpp)`.
3. For each frame:
   - try `c.run(frame_path=...)` with timeout=15s.
   - record return: count VAR, error VAR, exception, timeout.
   - immediately try `c.ping()` — record whether backend is alive.
4. After loop:
   - `c.ping()` — must succeed.
   - `c.list_instances()` — `cnt` must still be present.
   - `c.exchange_instance("cnt", {"command":"set_threshold","value":999})`
     — bumps the limit so even a 12-blob frame is "happy".
   - One final `c.run(frame_path=<a previously-crashing frame>)` — must
     return the right count.
5. Print summary table + write `RESULTS.md`.

### Acceptance
- All 7 happy frames return correct count (= ground-truth blob_count).
  If the worker is mid-respawn the SDK call may surface as a transient
  error — accept ONE retry per happy frame after a crash.
- 0% of crashes propagate: `c.ping()` after every frame must succeed.
- Post-`exchange`/post-recovery happy run on a previously-crashing frame
  must return its real count.

## Open questions / assumptions

1. **Will `c.run` throw, time out, or return with an `error` VAR when the
   plugin process()s segfault inside the worker?** Docs hint that
   `ProcessInstanceAdapter` "auto-respawns... and replays the last
   `set_def`". Best guess: the in-flight call comes back as an SDK
   `ProtocolError` (named-pipe broken or worker-crashed event), then the
   next call works because the adapter has re-spawned. We'll log whichever
   shape happens.

2. **Does `compile_and_load` happen relative to the project, or do we have
   to invoke it explicitly?** `open_project` compiles project plugins
   only (per `client.py` docstring), not the script. We'll call
   `compile_and_load(inspect.cpp)` afterwards as in `circle_size_buckets`.

3. **`call_timeout_ms` default.** docs say 30s for isolated instances.
   We'll leave it default — a segfault doesn't hang, it crashes
   immediately, so the timeout shouldn't fire.

4. **What does the contradiction between adding-a-plugin.md ("same-process
   plugins protected by `_set_se_translator`") and instance-model.md
   ("default: process") mean for behavior?** Either the docs disagree or
   the guide is stale. Will note in friction log if observed behavior
   matches one over the other. The guide also says "shm-process-isolation
   spike on its branch... opt-in" which doesn't match instance-model.md
   ("default: process"). Either the default flipped and adding-a-plugin.md
   is stale, OR the worker env isn't always set up so the fall-back to
   in-proc path is the real default. We'll find out.

5. **Inspect.cpp shape.** Use `xi::current_frame_path()` + `xi::imread()`
   pattern (from `circle_size_buckets`), feed into `xi::use("cnt")`, emit
   `count` and `mask` VARs.

6. **Crash style.** Null deref over div-by-zero: div-by-zero on x64 only
   traps for integer divide; floating div-by-zero produces inf without a
   trap. Null deref is a deterministic SEH access violation on Windows —
   simplest hard crash that bypasses any throw/catch.

## Files I will create

```
examples/crash_recovery/
  PLAN.md
  project.json
  inspect.cpp
  generate.py            # 10 frames + ground_truth.json
  driver.py
  RESULTS.md             # written by driver
  ground_truth.json      # written by generate.py
  frames/                # written by generate.py
  plugins/count_or_crash/
    plugin.json
    src/plugin.cpp
  instances/cnt/instance.json
```
