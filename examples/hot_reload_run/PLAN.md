# PLAN — hot_reload_run (FL r4)

## Goal
Validate the framework's hot-reload-during-continuous-run promise:
- `cmd:start fps=N` produces a stream of `vars` events
- While that's running, swap inspect.cpp on disk and call `compile_and_load`
- The run continues, `xi::state()` survives, `xi::Param<int>` survives.

## Approach

1. Author `inspect_v1.cpp`:
   - file-scope `xi::Param<int> threshold{"threshold", 100, {0, 1000}}`
   - inside entry: load `xi::state()["count"].as_int(0)`, +1, store
   - VARs: `count`, `threshold`, `triggered`
2. Author `inspect_v2.cpp`:
   - same threshold Param, same state["count"] handling
   - extra VAR `version=2`, extra VAR `half_count`
3. Driver `run.py`:
   - copy v1 -> `inspect.cpp`, `compile_and_load(inspect.cpp)`
   - read+drain initial state; bump threshold via `set_param` to a sentinel value (e.g. 137) so we can verify it survives reload
   - `c.call("start", {"fps": 20})`
   - background-thread drain `_inbox_vars` queue, timestamp every msg, store dict items by name
   - sleep ~2s
   - record reload boundary: copy v2 onto disk as `inspect.cpp`, mark `t_pre_reload`, call `compile_and_load`, mark `t_reload_returned`
   - sleep ~2s more
   - `c.call("stop")`, drain remaining vars events
4. Analyse:
   - split events at `t_reload_returned`
   - compare last pre count vs first post count (state survived if first_post >= last_pre)
   - check first post event with `version == 2` and how many post-events before it
   - check threshold value before vs after
   - compute inter-event gaps; report largest, especially the one straddling reload
   - `c.ping()` after stop

## Open questions
- Does `compile_and_load` block while continuous loop is running, and does the continuous loop survive across the reload (vs being stopped)?
  - Source check: `service_main.cpp:1086` shows compile_and_load STOPS continuous mode before reload (sets g_continuous=false). So in fact the continuous loop is torn down. The question is whether the backend re-starts it automatically, or whether the driver has to call `start` again. **I expect to find that we lose the run across reload** unless the backend silently re-arms it.
  - This is a likely friction point. The case spec says "Keep cmd:start running across the boundary — do NOT call cmd:stop", which the test driver enforces, but the backend itself may stop.
- How are vars events delivered? `client._inbox_vars` is a Queue populated by the read thread for every `vars` text frame — so I just consume it on a worker thread.
- `state_dropped` event? Both v1 and v2 omit `XI_STATE_SCHEMA(...)` so version stays 0 on both — backend should keep state.
