# PLAN — crash_recovery3

## Goal
Validate xInsp2's plugin crash isolation: a plugin that deliberately segfaults
on some frames must not bring down the backend. Subsequent frames (happy or
crashing) must be servable; `c.ping()` must keep working; the instance must
remain registered; and after `exchange_instance` raises the threshold, a final
happy run must return the right count.

## What I learned from the docs

- `instance-model.md` says **default isolation is `process`** — every instance
  runs in `xinsp-worker.exe`. The worker wraps `process()` with
  `_set_se_translator`, so a SEH (AV / null deref / div0) is caught and
  surfaced as a per-call error in the returned Record (`error` field set).
  Worker stays up, the next call goes through.
- `adding-a-plugin.md` confirms: the script-side sees `out["error"]` populated
  on a crash and can keep going. So the failure mode is "this one process()
  call returned an error", not "next call hangs / pipe is dead".
- If the worker process itself dies (corrupting things SEH can't catch),
  the adapter respawns it (rate-limited 3/60s) and replays last `set_def`.
- Per-instance opt-out via `"isolation": "in_process"` — I will NOT use this.
  The whole point is to verify the default.
- `plugin-abi.md`: I'll use `XI_PLUGIN_IMPL` and inherit from `xi::Plugin`.
- `writing-a-script.md`: script reads the input frame via `xi::imread(xi::current_frame_path())`.
- `client.py`: `compile_and_load`, `open_project`, `exchange_instance` all
  raise `ProtocolError` on backend nope; plain `c.run()` returns a `RunResult`
  whose vars carry whatever the script `VAR()`'d.

## Approach

### Plugin: `count_or_crash`
- Inherits `xi::Plugin`.
- Config: `crash_when_count_above` (int, default 8).
- `process(input)`:
  1. Threshold input image "src" at fixed level (e.g. 128) into a binary mask.
  2. `cv::connectedComponents` on the mask, get `n_labels - 1` (subtract bg).
  3. If count > threshold → `*(volatile int*)0 = 42;` — real null deref,
     not a throw. SEH-translatable, distinct from any C++ exception path.
  4. Else return `{ "count": int, "mask": image }`.
- `exchange({command:"set_threshold", value:N})` updates threshold live and
  returns `get_def()` per convention.
- `get_def` / `set_def` round-trip threshold + telemetry.

### Test data
Generate 10 320x240 grayscale PNGs with noisy gradient bg. Place 5 or 12
bright blobs (white circles ~12 px radius) on a noisy mid-gray background
chosen so threshold=128 cleanly separates blobs from bg.
- 5 happy frames (5 blobs each).
- Wait — spec says: at least 3 crashing, at least 5 happy. I'll do 6 happy
  + 4 crashing. Mix the order so a crash sits between two happy frames AND
  two crashes happen in a row (verify worker survives multiple crashes).
- `ground_truth.json` with `[{file, blob_count}, ...]`.

### Script (`inspect.cpp`)
- Read frame via `xi::imread(xi::current_frame_path())`.
- Convert RGB→GRAY (decoder gives RGB).
- `xi::use("counter").process(Record().image("src", gray))`.
- `VAR(count, …)`, `VAR(error, …)`, `VAR(crashed, error_str.empty()?false:true)`.

### Driver
- `open_project(...)`, `compile_and_load(inspect.cpp)`.
- For each frame in ground truth:
  - `c.run(frame_path=...)`.
  - Read `count` and `error` and `crashed` vars.
  - Then `c.ping()` to verify backend alive.
- After loop:
  - `c.ping()` and `c.list_instances()` → instance still there.
  - `c.exchange_instance("counter", {command:"set_threshold", value:1000})`.
  - One final `c.run(frame_path=<a previously-crashing frame>)` — must return
    the correct (high) blob count.
- Print summary table; write `RESULTS.md`.

## Open questions / things I'll watch for

1. Does the `error` Record field from a worker SEH crash actually surface as
   a `VAR("error", ...)` on the script side, or does the script need to
   explicitly check `out["error"]`? — the script will explicitly check.
2. How long does the per-call timeout for an isolated instance behave on
   first crash? Default is 30s; my crash is instant so should not matter.
3. Is `c.run(frame_path=...)` blocked by the crash (pipe stuck) or does it
   return normally with the error baked in? Docs say latter; I'll measure.
4. Does `exchange_instance` work on an instance that just crashed N times?
   Should — worker process keeps running per docs.
5. Does the in-worker SEH wrapper actually exist in this build? The docs
   describe it as default-on; if not, this is a P0 finding.
