# PLAN — hot_reload_run (FL r4)

## Goal
Validate the framework's hot-reload-during-continuous-run promise:
- `cmd:start fps=N` produces a stream of per-run output (now surfaced via
  the `expose` sink, since VAR was removed from the core SDK)
- While that's running, swap inspect.cpp on disk and call `compile_and_load`
- The run continues, `xi::state()` survives, `xi::Param<int>` survives.

## Output path (post-VAR-purge)
VAR no longer publishes anything. The script pushes its values to the
`expose` plugin instance via `xi::use("expose").process(rec)` under
channel `main` (`rec.set("$channel", "main")`). The driver opens the
project (so the `expose` instance exists), `subscribe`s to the channel,
and drains+decodes the XEX1 binary frames the sink pushes — one per run —
using `examples/lib/xex1.py` (`collect_frames`).

## Approach

1. Author `inspect_v1.cpp`:
   - file-scope `xi::Param<int> threshold{"threshold", 100, {0, 1000}}`
   - inside entry: load `xi::state()["count"].as_int(0)`, +1, store
   - expose record on channel `main`: `count`, `threshold`, `triggered`
2. Author `inspect_v2.cpp`:
   - same threshold Param, same state["count"] handling
   - extra field `version=2`, extra field `half_count`
3. Driver `run.py`:
   - copy v1 -> `inspect.cpp`, `open_project` (creates the `expose`
     instance), `compile_and_load(inspect.cpp)`, `subscribe(["main"])`
   - read+drain initial state; bump threshold via `set_param` to a sentinel value (e.g. 137) so we can verify it survives reload
   - `c.call("start", {"fps": 20})`
   - background-thread drain the binary inbox via `collect_frames`,
     timestamp every frame, store its `values` dict
   - sleep ~2s
   - record reload boundary: copy v2 onto disk as `inspect.cpp`, mark `t_pre_reload`, call `compile_and_load`, mark `t_reload_returned`
   - sleep ~2s more
   - `c.call("stop")`, drain remaining frames
4. Analyse:
   - split frames at `t_reload_returned`
   - compare last pre count vs first post count (state survived if first_post >= last_pre)
   - check first post frame with `version == 2` and how many post-frames before it
   - check threshold value before vs after
   - compute inter-frame gaps; report largest, especially the one straddling reload
   - `c.ping()` after stop

## Open questions
- Does `compile_and_load` block while continuous loop is running, and does the continuous loop survive across the reload (vs being stopped)?
  - Source check: `service_main.cpp:1086` shows compile_and_load STOPS continuous mode before reload (sets g_continuous=false). So in fact the continuous loop is torn down. The question is whether the backend re-starts it automatically, or whether the driver has to call `start` again. **I expect to find that we lose the run across reload** unless the backend silently re-arms it.
  - This is a likely friction point. The case spec says "Keep cmd:start running across the boundary — do NOT call cmd:stop", which the test driver enforces, but the backend itself may stop.
- How is per-run output delivered? The `expose` sink pushes one XEX1 binary WS frame per run for subscribed channels; the SDK queues raw binary on `client._inbox_binary`, and `xex1.collect_frames` drains+decodes them — so I just consume them on a worker thread.
- `state_dropped` event? Both v1 and v2 omit `XI_STATE_SCHEMA(...)` so version stays 0 on both — backend should keep state.
