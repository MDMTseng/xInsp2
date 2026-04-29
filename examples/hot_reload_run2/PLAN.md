# hot_reload_run2 — PLAN

## Goal
Validate that under continuous mode (`cmd:start fps=N`), `compile_and_load`
swapping `inspect.cpp` v1 → v2 mid-run preserves:
1. `xi::state()["count"]` (state survives)
2. `xi::Param<int> threshold` (param replay)
3. continuous mode auto-resumes (rsp carries `resumed_continuous: true`)

## Files
- `inspect_v1.cpp` — initial script. VARs: count, threshold, triggered.
- `inspect_v2.cpp` — post-reload script. VARs: count, threshold, triggered, version=2, half_count.
- `inspect.cpp` — copy of whichever the driver wrote last.
- `driver.py` — runs the case end to end; writes RESULTS.md.
- `RESULTS.md` — friction log.

No plugins / instances needed — the script is pure state + count.

## Driver flow
1. Connect, ping.
2. Copy `inspect_v1.cpp` -> `inspect.cpp`. `compile_and_load(inspect.cpp)`.
3. Spawn a background thread to drain `_inbox_vars` (the SDK doesn't
   expose continuous-mode vars cleanly — `c.run()` would conflict).
   Each `vars` message: pull `count`, `threshold`, `version` (None on v1).
   Stamp wall-clock time so we can compute the largest gap.
4. `c.call("start", {"fps": 20})`.
5. Sleep ~2.0s collecting events.
6. `c.set_param("threshold", 137)`.
7. Sleep ~0.4s so we get a couple of post-set_param events while still v1.
8. Copy `inspect_v2.cpp` -> `inspect.cpp`. `c.compile_and_load(inspect.cpp)`.
   Capture rsp; check for `resumed_continuous: true`. Mark wall-clock
   moment of reload return.
9. Sleep ~2.0s collecting v2 events.
10. `c.call("stop")`.
11. `c.ping()` to confirm backend healthy.
12. Analyse:
    - total events; pre/post split based on first event whose `version==2`
      (or fallback: first event after reload-return wall-clock if v2 never
      shows up).
    - last_pre count vs first_post count
    - last_pre threshold vs first_post threshold
    - v2 within 5 events of reload? (count of post-reload events until
      first version=2)
    - largest inter-event gap; report whether it straddles the reload
13. Write RESULTS.md.

## Things to be careful of
- `client.py`'s `Client.run()` drains `_inbox_vars` — must NOT call run
  while in continuous mode; just read `c._inbox_vars` directly via a
  thread.
- compile_and_load timeout: SDK uses 180s. Should be fine.
- In v1, `version` VAR doesn't exist — store None for that field.
- `xi::state()["count"]` — load with `as_int(0)`, `set("count", n+1)`.
- Param names in the JSON args use string types — the param decl is
  `xi::Param<int> threshold{"threshold", 100, {0, 1000}}`. `set_param`
  passes it as a JSON value, which the backend stringifies.
- v1 and v2 must NOT carry `XI_STATE_SCHEMA(...)`, so neither side
  bumps schema and `state_dropped` will not fire (we want state to
  survive).
- Post-reload, the worker may take a beat to settle — the largest gap
  of the run will be the cl.exe + reload window (a few seconds is fine).

## Acceptance
- ≥30 vars events
- last_pre count ≤ first_post count
- v2 within 5 events of reload
- last_pre threshold == first_post threshold (== 137)
- ping after stop ok
- `resumed_continuous: true` in compile_and_load rsp; no manual restart needed
