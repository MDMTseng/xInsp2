# crash_recovery2 — PLAN

## Goal
Validate that the xInsp2 framework's process-isolation crash recovery
absorbs a real (SEH) crash inside a plugin's `process()` without taking
the backend down, and that subsequent calls succeed.

## Approach

1. **Plugin `count_or_crash`**
   - Threshold src grayscale, run `cv::connectedComponents` to count
     blobs.
   - If `count > crash_when_count_above` (default 8) → deliberate
     null-pointer write `*(volatile int*)0 = 42` (real SEH access
     violation, not a C++ throw).
   - Otherwise return `{count: int, mask: image}`.
   - `exchange()` accepts `{command:"set_threshold", value:int}` to
     update `crash_when_count_above` live; returns `get_def()`.
   - `get_def`/`set_def` round-trip the field.

2. **Test data** — 10 frames 320x240 grayscale gradient + noise
   - 5 happy frames with ~5 blobs each.
   - 5 crash frames with ~12 blobs each (count above default 8).
   - At least 3 crashing, at least 5 happy → satisfied.
   - Mixed order so we exercise crash-then-happy AND happy-then-crash.
   - Ground-truth JSON `{file, blob_count}` per frame.

3. **inspect.cpp** — orchestration only:
   - `xi::imread(xi::current_frame_path())`
   - call `det.process(...)` (instance of `count_or_crash`)
   - emit VARs: `count`, `mask`, `frame_path`. If the det Record has
     `error` set (the framework's per-call crash signal per docs), emit
     a `crashed` VAR.

4. **driver.py**
   - open_project + compile_and_load.
   - Loop over GT frames. For each: c.run(frame_path=...). Record:
     `truth_count`, `observed_count`, `crashed?` (run had an `error`
     VAR or no `count` VAR), `next_call_ok?` (set after the next
     iteration succeeds).
   - After loop: c.ping(), c.list_instances() (must include "det").
   - exchange_instance("det", {command:"set_threshold", value:1000})
     to bump threshold above any observed count.
   - Run one final happy frame; assert correct count.
   - Print summary table; emit RESULTS.md as a friction log.

## Open questions about framework crash handling

From `docs/reference/instance-model.md` "isolation modes":
- Default is `"process"` isolation. The worker has an SEH wrapper
  around `process()` and converts AVs into a per-call error in the
  reply Record.
- Backend logs `[xinsp2] use_process('<name>') isolated: plugin
  crashed: <reason>`; script side gets a Record with `error` set.
- A second-tier respawn fires only if the worker process itself dies.

Things I am NOT sure about until I run it:
- Does the in-script `det.process(...)` Record actually surface the
  `error` field in a way the script can VAR out to the Python SDK?
  i.e. does the script see a Record with `error` and an empty image,
  and what does my downstream code do with `record.get_image("mask")`
  on a crashed call?
- Does `c.run()` itself succeed when the plugin crashed, or does the
  whole inspection get marked failed?
- Do I need to set anything special in `instance.json` to enable
  process isolation? Docs say it's the default; I'll trust that.
- Whether `xinsp-worker.exe` is available in this build (docs say the
  backend falls back to in-proc if not, with a warning on open).

## Acceptance criteria
- All under-budget frames → correct `count`.
- All over-budget frames → reported as crashed but backend survives.
- c.ping() works after the crash storm.
- exchange_instance + final happy run returns correct count with the
  threshold bumped.
