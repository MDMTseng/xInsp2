# QA Agent EDGE — resource / security / edge-case plan

Viewpoint: resource exhaustion, security, and edge cases for the FE/BE split.
Maps to [`fe-be-split-test-plan.md`](../../docs/design/fe-be-split-test-plan.md)
§1 (CR-U* crash-report parsing), §3 (FE-E3 orphan guarantee), §5 (SP5 no
orphan). Complements — does **not** duplicate — `examples/fe_supervisor`
(crash-storm loop), `examples/fe_supervisor_healthy` (happy path),
`examples/plugin_crash_forensics` (BE-side minidump forensics), and
`backend/tests/test_safe_state.cpp` (sink contract).

Status legend: ✅ implemented here · 📄 documented check · ⏭ SKIP on non-nt.

---

## A. Crash-report parser edges — C++ unit (`backend/tests/test_qa_edge.cpp`)

Against the **proposed** portable `xi::enrich_from_crash_report`
(`backend/include/xi/xi_crash_report.hpp`, extracted from `fe_main.cpp`). Each
case writes a self-contained `be.log` + sibling `.dmp`/`.json` into a unique
temp dir so the minidump path the log names resolves to a real file — the exact
runtime contract. CHECK(cond) + CHECK_NOTHROW harness, mirrors
`test_safe_state.cpp`.

| ID | Proves | Pass criteria |
|---|---|---|
| CR-U1 ✅ | known-good report | `exception_name=ACCESS_VIOLATION`, `module=plugin_v0.dll`, `last_phase=inspect`, `report_path` ends `.json` |
| CR-U2 ✅ | `context.last_phase` empty → `threads[]` fallback | picks `inspect` even when an earlier thread breadcrumb (`idle`) precedes it |
| CR-U3 ✅ | be.log has NO `minidump:` line | no throw; `report_path` + all forensic fields empty |
| CR-U3b ✅ | be.log file missing entirely | no throw; everything empty |
| CR-U4 ✅ | sibling `.json` MISSING | `report_path` set (operator can look); other fields empty; no throw |
| CR-U4b ✅ | sibling `.json` CORRUPT | `report_path` set; forensic fields empty; no throw (cJSON null) |
| CR-U5 ✅ | multiple `minidump:` lines | LAST wins — reads `new.dmp`/`new.json`, not the stale first |
| CR-U6 ✅ | path with spaces + trailing CR/ws | trims correctly; `report_path` is the `.json` sibling; no throw |

Crafted standalone fixtures (illustrative, human-readable) under `fixtures/`:
`good/`, `threads_fallback/`, `no_minidump/`, `multi_minidump/`, `corrupt_json/`.

## B. Orphan / leak / quoting / security edges — Python driver (`driver.py`)

Need a live FE; uses PRIVATE ports **7910-7913** (+7916 for QE-L1). ⏭ SKIP on
non-nt.

| ID | Proves | Pass criteria |
|---|---|---|
| QE-O1 ✅⏭ | hard `taskkill /F` of FE (FE-E3 / SP5) | Job Object `KILL_ON_JOB_CLOSE` reaps BE; port `:7910` closed afterward — no orphan |
| QE-O2 ✅⏭ | FE killed DURING respawn-backoff | kill lands in the 1.5 s gap; still no orphan on `:7911` |
| QE-L1 📄⏭ | no growing handle leak across many respawns | FE `HandleCount` roughly flat early vs late across a crash-storm; soft-skip if no tooling (re-run with handle.exe / Process Explorer) |
| QE-Q1 ✅⏭ | `--project` path WITH SPACES launches (`build_cmdline` quoting) | BE boots, opens port `:7912` — quoted path arrived as one argv token |
| QE-S1 ✅⏭ | `--project` shell-injection value is a PATH, not a shell | value `bogus" & calc.exe & "tail` spawns **no** calc.exe; no orphan (CreateProcessA → no cmd.exe) |

## C. Safety property coverage

- **SP5** (FE never leaves an orphan): QE-O1 + QE-O2 + every driver's post-exit
  port check.
- Negative-path robustness (CR-U3/U3b/U4/U4b): the parser is on the BE-death
  path, so a throw would abort going safe — `CHECK_NOTHROW` guards every case.

## Run

```
# C++ unit (after the integrator adds the CMake lines — see report):
ctest --test-dir backend/build -C Release -R test_qa_edge
#   or directly: backend/build/Release/test_qa_edge.exe

# Live edge driver (Windows; private ports 7910-7913):
python examples/qa_edge/driver.py
```
