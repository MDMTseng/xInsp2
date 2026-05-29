# QA team loop — Round 1 (countdown 10 → 9)

4 QA agents, distinct viewpoints, each authored a test plan + implemented tests
in a disjoint namespace; I integrated centrally (CMake + parser extraction),
built, ran, and fixed.

## Agents & deliverables
| Agent | Viewpoint | Artifacts |
|---|---|---|
| FUNC | functional / lifecycle | `examples/qa_func/` (driver.py, 7 cases AS-I1/2/3/4/7 + FE-E9/E10, fixtures) |
| FAULT | reliability / fault-injection | `examples/qa_fault/` (4 drivers: autostart negatives, bad-exe, recover-and-clear, respawn accounting) + `backend/tests/test_qa_fault.cpp` (cap/window math) |
| RACE | concurrency / timing | `examples/qa_race/` (driver, ctrlbreak, fe4-stub) + `backend/tests/test_qa_race.cpp` (cap/window + forensics-at-cap) |
| EDGE | resource / security / edge | `examples/qa_edge/` (driver + crafted crash-report fixtures) + `backend/tests/test_qa_edge.cpp` + **`backend/include/xi/xi_crash_report.hpp`** (extracted parser) |

## Integration done centrally
- Wired the extracted crash-report parser into production: `fe_main.cpp` now
  includes `xi_crash_report.hpp` and calls `xi::enrich_from_crash_report`
  (removed the file-static duplicate) — so the EDGE unit tests the REAL code path.
- Added `test_qa_fault` / `test_qa_race` / `test_qa_edge` to CMake. **ctest now 11/11.**

## Product issues FOUND & FIXED this round
1. **UTF-8 em-dashes in FE output** → mojibake / `UnicodeDecodeError` under the
   operator's **cp950** console locale (broke `--help` capture). Fixed the
   emitted `fe_main.cpp` strings to ASCII (`-`). Real cross-locale robustness bug.
2. **No readiness signal — "port-up ≠ ready".** The BE binds its WS port, then
   runs open/compile/start *synchronously* for ~8 s before accepting; the port
   reads "up" the whole time. Added a `[xinsp2] autostart: ready` marker after
   the sequence so the FE / operator / tests can tell *listening* from *serving*.
3. **Dead "open only" path + wrong relative-`--script` base.** `open_project`
   always defaults `script_path` to `<folder>/inspection.cpp`, so the no-script
   branch never fired (autostart fired a doomed compile); and a relative
   `--script` was resolved against the backend's cwd, not the project. Fixed:
   relative `--script` → resolved under `--project`; a missing script *file* →
   genuine open-only.
4. **Observability** (carried from solo round 2): FE now announces
   `backend healthy` once per instance, not only after a prior safe-state.

## Test bug found & fixed
- `test_qa_fault` QF-U1 loop bound `t < 100` (step 1500) ran once → cap never
  reached. Fixed to `t < 100'000`. (The `on_death` model matched `fe_main`;
  only the driver bound was wrong.)

## Result
- All 4 QA suites PASS. ctest 11/11. Regression: `plugin_crash_forensics`,
  `fe_supervisor`, `fe_supervisor_healthy` all still PASS after the
  service_main / fe_main changes.

## Open / deferred (for later rounds)
- **FE-E4 PortUnresponsive** can't be triggered by a hang plugin (inspects run
  on detached threads; the WS poll thread keeps accepting). RACE documented a
  faithful trigger: a BE `--hang-after-boot` debug hook that stops calling
  `srv.poll()`. Stub SKIPs until that hook exists. → candidate next round.
- **FE treats port-up as healthy** even during the ~8 s autostart window (logs
  "backend healthy" before the line truly serves). Deep fix = FE waits for
  `autostart: ready` in be.log. Phase-2 (deep heartbeat).
- Safe-state log still omits `ts_ms` (carried from solo round 1).
