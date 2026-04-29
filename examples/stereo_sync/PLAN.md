# stereo_sync — PLAN

## Goal
Validate the framework's TriggerBus correlation across **two distinct
source plugin instances** that emit independently with the same `tid`.

## Approach

### Plugin `synced_cam`
- Single plugin type, instantiated twice as `cam_left` / `cam_right`.
- C++ project plugin (`plugins/synced_cam/src/plugin.cpp` + `plugin.json`
  with `has_ui: true` + minimal `ui/index.html`).
- Worker thread inside each instance generates 320x240 grayscale frames
  every ~50 ms (target 20 Hz).
- Embeds the running sequence number in the first 4 bytes of pixel data
  (little-endian uint32).
- Emits ONE image per call to `host->emit_trigger(name, tid, ts, &img, 1)`
  — single-image-per-source convention so the bus key collapses to the
  instance name (`"cam_left"` / `"cam_right"`).
- TID is derived deterministically from the sequence number so both
  instances generate the *same tid* for the same `seq`:
  `tid.hi = 0xC0FFEE; tid.lo = (uint64_t)seq + 1`.
  (We avoid `XI_TRIGGER_NULL` because that lets the bus auto-allocate a
  random tid, and we need both cameras to agree.)
- `ts` left as 0 (host fills in).
- `get_def`: rate (fps), seq counter, running flag.
- `set_def`: fps tunable.
- Worker started in constructor (no laziness needed); stopped + joined
  in destructor.

### Project config
- `project.json` declares `trigger_policy = "all_required"` with
  `required: ["cam_left", "cam_right"]` and a generous `window_ms`
  (say 500 ms) so emits-out-of-order across the IPC boundary still
  correlate.
- Two instances seeded under `instances/cam_left/` and
  `instances/cam_right/` referencing plugin `synced_cam`.
- Script `inspect.cpp`.

### Script `inspect.cpp`
```
auto t = xi::current_trigger();
if (!t.is_active()) return;
auto L = t.image("cam_left");
auto R = t.image("cam_right");
// decode seq from first 4 bytes
VAR(left_seq, ...); VAR(right_seq, ...); VAR(matched, l==r);
```

### Driver `driver.py`
- `open_project()`, `compile_and_load("inspect.cpp")`.
- `c.exchange_instance("cam_left", {"command":"start"})` — actually,
  we'll just have the plugin start the worker in its ctor. So once
  `open_project` returns, both workers are already streaming.
- `c.call("start", {fps: 25})` — turn on continuous mode so the worker
  thread inside the backend starts dispatching trigger events to the
  script.
- Sleep ~2 s, observe `vars` events accumulated on `c._inbox_vars`.
- Compute matched-rate. Acceptance: >=95% matched, >=30 cycles.
- Run a 5 s soak: same scheme, sleep 5 s, expect ~80–120 cycles, check
  backend still alive (ping at the end).

## Open questions / risks

1. **Process isolation + emit_trigger.**
   Plugins default to `process` isolation — the worker thread runs
   inside `xinsp-worker.exe`, but the TriggerBus singleton lives in the
   backend process. Does `host->emit_trigger` properly cross the IPC
   boundary back to the backend's bus? The docs say "host services"
   but the IPC reference `ipc-shm.md` is what would confirm this.
   *Mitigation:* set `"isolation": "in_process"` on both instances —
   safer bet, the trigger bus is in-process for sure. The case spec
   permits this and explicitly mentions emit_trigger crossing the
   process boundary, so the framework SHOULD support it; if it doesn't
   that's a real friction point worth recording.
   **Decision:** start with default (process). If it fails, switch to
   in_process and write up the F-N entry.

2. **TID collision strategy.**
   Both instances must produce identical tids per cycle. Using
   `seq -> tid.lo` is simple. But the workers run as separate threads
   in separate processes — clock skew + scheduler jitter could mean
   `cam_left` is on seq=10 while `cam_right` is on seq=8. The
   AllRequired bus correlator caches partials within `window_ms`, so as
   long as both eventually emit `tid=10` within the window they pair
   up. With `window_ms=500` and 50 ms cycle time we have ~10 cycles of
   slack.

3. **Cycle count target.**
   At ~20 Hz / 50 ms cycle, 30 cycles = 1.5 s. We'll wait 2.5 s to give
   plenty of headroom.

4. **VAR redeclaration gotcha.** Script will use distinct VAR names for
   each step to avoid the macro-collision pitfall.

5. **continuous mode subscription.** `c.run()` only handles one-shot
   inspect; for bus-driven dispatches we need to consume `vars`
   messages off `c._inbox_vars` directly while `start` is active —
   same shape as `runMulticam.mjs`.

## Build steps
1. Write plugin sources + plugin.json + ui/index.html.
2. Write script inspect.cpp.
3. Write project.json and seed instance.json files.
4. Write driver.py.
5. Run driver. Iterate. Write RESULTS.md.
