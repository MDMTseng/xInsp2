# FE/BE split — test plan

Companion to [`fe-be-split.md`](./fe-be-split.md). Covers the frontend
supervisor (`xinsp-fe.exe`), the `SafeStateSink` seam, the backend headless
autostart CLI, and the VS Code `backendMode` — plus the backend crash forensics
the FE depends on.

This is a **safety component**: when it fails, a machine keeps running with a
dead inspector. So the plan weights the *negative* and *ordering/timing*
properties as heavily as the happy path. Items are tagged **[DONE]** (a test
exists today), **[TODO]** (specified here, not yet built), or **[MANUAL]**.

Status legend for the gap summary at the end: ✅ covered · 🟡 partial · ❌ gap.

---

## 0. Scope & what each layer owns

| Layer | Under test | Primary mechanism |
|---|---|---|
| `xi_safe_state.hpp` | sink contract, formatting, factory | C++ unit (`ctest`) |
| `service_main.cpp` autostart | `--project/--script/--autostart-fps` | integration driver + node suite |
| `fe_main.cpp` (`xinsp-fe.exe`) | spawn / monitor / safe-state / respawn / shutdown | example driver (`driver.py`) |
| `extension.ts` `backendMode` | managed / attach / auto, no-double-supervise | node + extension-host e2e |
| BE crash forensics (existing) | minidump + report the FE reads | `plugin_crash_forensics` (✅) |

Out of scope (Phase 2, not built — no tests yet): C++ WS-client deep heartbeat,
FE status channel, real PLC transports, Linux supervisor.

---

## 1. Unit — `SafeStateSink` (`test_safe_state`, new ctest binary)

Pure logic; no process spawning. Use the repo's `CHECK(cond)` harness (see
`backend/tests/test_diagnostics.cpp`), capture sink output with `tmpfile()`.
Register in `backend/CMakeLists.txt` (`add_executable` + `add_test`), add to
`docs/testing.md`.

| ID | Proves | Pass criteria |
|---|---|---|
| SS-U1 | `to_string(SafeStateReason)` | each enum → exact string; out-of-range → `"Unknown"` |
| SS-U2 | factory fallthrough | `make_safe_state_sink("log")`, `("")`, `("modbus")` all return non-null, `name()=="log"` |
| SS-U3 | `enter_safe_state` formatting | captured line contains reason, `rc=0x%08X`, module, phase, report |
| SS-U4 | `clear_safe_state` | captured line is the `CLEAR SAFE STATE` form |
| SS-U5 | empty fields | empty module/phase/report render as `-` (the `?:` placeholders) |
| SS-U6 | overflow safety | 4 KB module/report string → no crash, truncates within the 512 buf |
| SS-U7 | null `FILE*` | `LoggingSafeStateSink(nullptr)` enter/clear → no crash (stderr-only path) |

**Refactor to enable a unit:** the FE's crash-report parser
(`enrich_from_crash_report` in `fe_main.cpp`) is currently a file-static
function, so it can't be unit-tested in isolation. **[TODO]** extract it (and
the `minidump:` regex) into a small portable header (e.g.
`xi_crash_report.hpp`) and add:

| ID | Proves | Pass criteria |
|---|---|---|
| CR-U1 | parse a known-good report | given a fixture `be.log` + sibling `.json`, fills `exception_name`/`module`/`last_phase` |
| CR-U2 | `context.last_phase` empty → `threads[]` fallback | picks the `inspect` breadcrumb from `threads[]` |
| CR-U3 | no `minidump:` line in log | leaves event fields empty, `report_path` empty, no throw |
| CR-U4 | report `.json` missing/corrupt | `report_path` set, other fields empty, no throw |
| CR-U5 | last-match wins | multiple `minidump:` lines → uses the last (most recent crash) |

---

## 2. Integration — backend headless autostart

Drive `xinsp-backend.exe` directly (private port, log to file), no FE. Mirror
the `examples/plugin_crash_forensics/driver.py` structure. **[TODO]** as a new
runner (`vscode-extension/test/runAutostart.mjs` or an example driver).

| ID | Proves | Pass criteria |
|---|---|---|
| AS-I1 | open+compile a healthy project | log shows `autostart: open_project` then `compile_and_load`; project opens; WS port accepts |
| AS-I2 | `--autostart-fps=N` starts continuous | a client attaching sees `dispatch_stats` advancing / vars flowing |
| AS-I3 | `--script=PATH` override | the explicit script compiles, not project.json's |
| AS-I4 | `--project` with no script | logs `no script … open only`; **process stays up**; port accepts |
| AS-I5 | nonexistent `--project` dir | `open_project` fails, error logged, **BE does not exit**, port still up |
| AS-I6 | project with a bad (won't-compile) script | `compile_finished` error; **BE stays up**; a client can still drive it |
| AS-I7 | client attaches after autostart | WS not monopolised — `version`/`ping`/`run` all work for the late client |
| AS-I8 | no `--project` (regression) | identical to pre-change boot (the existing `ws_*` suites stay green) |

---

## 3. Integration / E2E — `xinsp-fe.exe` supervisor

Example dir `examples/fe_supervisor/` (driver launches the FE, asserts on the
FE log + process/port state). FE-E1 exists; the rest are **[TODO]**.

| ID | Proves | Pass criteria |
|---|---|---|
| FE-E1 **[DONE]** | crash-storm safety loop | death detected → `enter_safe_state` w/ forensics → respawn → cap → stays safe → no orphan (existing `driver.py`) |
| FE-E2 **[TODO]** ⭐ | **happy path** (the critical missing positive) | healthy project: BE boots, FE logs `backend healthy` + `CLEAR SAFE STATE`, runs ≥10 s with **zero** `ENTER SAFE STATE`, Ctrl-C → clean exit, no orphan |
| FE-E3 **[TODO]** ⭐ | **orphan guarantee** | hard-`taskkill` the FE (not Ctrl-C) → Job Object `KILL_ON_JOB_CLOSE` reaps the BE; no `xinsp-backend` on the port afterward |
| FE-E4a **[DONE]** | boot hang | BE bound but never reaches `autostart: ready` within `--boot-timeout-ms` → `enter_safe_state(BootTimeout)` → respawn → cap. `examples/qa_race/driver_boot_hang.py` (uses the BE `--hang-before-ready` hook). |
| FE-E4b **[Phase 2]** | serve-time wedge | BE alive + port still accepting but commands stall — the shallow connect probe *cannot* see this; needs a WS handshake/ping heartbeat. `driver_fe4.py` documents the trigger and stays a stub. |
| FE-E5 **[TODO]** ⭐ | **recover-and-clear transition** | project that crashes once then runs healthy after respawn → exactly one `ENTER`, then `CLEAR SAFE STATE`, then stable; FE keeps running (no cap) |
| FE-E6 **[TODO]** | backend exe missing | `--backend=<bad>` → `CreateProcess` fails → `enter_safe_state(BackendExit)` + FE exits rc=1 |
| FE-E7 **[DONE via FE-E1]** | forensics from `threads[]` fallback | safe-state line carries `phase=inspect` even though the crash is on an unmanaged thread (empty `context`) |
| FE-E8 **[MANUAL/slow]** | respawn window prunes | crashes spaced >60 s apart never trip the cap (sliding window) |
| FE-E9 **[TODO]** | `fe.json` config + CLI override | flags honoured from `fe.json`; a CLI flag overrides the same key |
| FE-E10 **[TODO]** | `--help` | exits 0, prints usage, spawns nothing |

---

## 4. Node / extension-host — `backendMode`

The extension's mode logic (`isPortOpen` probe + attach guard) is best covered
by a focused unit of the resolution function plus an extension-host e2e for the
spawn-guard. All **[TODO]**.

| ID | Proves | Pass criteria |
|---|---|---|
| EX-N1 | `managed` (regression) | extension spawns a backend, respawns on crash (today's behavior unchanged) |
| EX-N2 | `attach` + pre-running BE | extension connects; process count of `xinsp-backend` stays at the one we pre-spawned (no second spawn) |
| EX-N3 | `attach` + BE disappears | extension does **not** respawn; health item shows `xInsp2 · safe`; no spawn attempt in the log |
| EX-N4 | `auto` resolution | port open → attaches (no spawn); port closed → managed (spawns) |
| EX-N5 | `restartBackend` in attach | info toast, reconnect only, **no** kill/spawn |
| EX-N6 | crash toast wording by mode | managed = "recovered/respawning"; attach = "FE recovering, line safe" |

---

## 5. Safety properties (assert in addition to functional pass)

These are the reasons the component exists; verify explicitly, mostly by
asserting **order and timing** in the FE log.

| ID | Property | How to assert |
|---|---|---|
| SP1 | safe-state is driven **before** respawn | `ENTER SAFE STATE` line precedes the matching `respawning backend` line |
| SP2 | safe-state on **every** death incl. the cap one | count of `ENTER SAFE STATE reason=BackendExit` == number of deaths; plus one `RespawnLimitExceeded` |
| SP3 | bounded "dead but not safe" latency | timestamp(`ENTER SAFE STATE`) − timestamp(BE exit) ≤ probe interval + margin |
| SP4 | `CLEAR` only after **confirmed** healthy | every `CLEAR SAFE STATE` is preceded by a successful port probe, never optimistic |
| SP5 | FE never leaves an orphan | FE-E3 + every driver's post-exit port check |
| SP6 | at the cap, FE **stays safe** (doesn't spin) | after `RespawnLimitExceeded`, no further `respawning` lines; FE exits or idles in safe state |

---

## 6. Regression gates (must stay green on every change)

- `ctest --test-dir backend/build -C Release` → 7/7 (will be 8/8 after `test_safe_state`).
- `examples/plugin_crash_forensics/driver.py` → PASS (BE forensics the FE reads).
- `examples/fe_supervisor/driver.py` → PASS.
- `examples/cross_proc_trigger` + `multi_source_surge` → PASS (prove plain boot, no `--project`, is unaffected).
- `vscode-extension` `node --test test/*.test.mjs` → green (default-boot path unchanged).
- `node esbuild.mjs` builds the extension clean.

---

## 7. Cross-platform

The FE supervisor, the Win32 spawn/probe, and the autostart are Windows-only
today (gated `#ifdef _WIN32`). `examples/fe_supervisor/driver.py` **SKIPs** on
non-`nt`. When the Linux supervisor lands (`posix_spawn`/`PR_SET_PDEATHSIG`/
POSIX probe — see [`linux-port.md`](./linux-port.md)), every §3 item must be
re-run on Linux; §1 (`SafeStateSink`/parser) is already portable and its units
run on both.

---

## 8. Coverage snapshot & priority gaps

| Area | State |
|---|---|
| BE crash forensics (minidump + report) | ✅ `plugin_crash_forensics` |
| FE crash-storm → safe-state/respawn/cap/orphan | ✅ `fe_supervisor` (FE-E1) |
| C++ unit `ctest` regression | ✅ 7/7 |
| `SafeStateSink` unit + crash-parser unit | ❌ SS-U*, CR-U* |
| **FE happy path / clean shutdown** | ❌ FE-E2 |
| **FE orphan-kill guarantee** | ❌ FE-E3 |
| **FE recover-and-clear transition** | ❌ FE-E5 |
| Autostart negative cases (bad dir/script/no-script) | ❌ AS-I4/5/6 |
| Extension attach mode decision (managed/attach/auto) | ✅ `backend_mode.test.mjs` |
| Compile-broken line not reported healthy (gap #1) | ✅ `qa_fault/driver_fe_degraded.py` (BE `autostart: degraded` → FE safe) |
| Respawn cap/window math (real fn, not a copy) | ✅ `xi_respawn_policy.hpp` unit-tested by `test_qa_fault`/`test_qa_race` |
| Boot hang (FE-E4a) | ✅ `qa_race/driver_boot_hang.py` |
| Serve-time wedge (FE-E4b), extension-host e2e, soak/leak, Linux | ❌ Phase 2 / infra |

**Build next, in order:** (1) FE-E2 happy path + FE-E3 orphan — the two
highest-value safety holes; (2) `test_safe_state` (SS-U*) — cheap, fast, guards
the contract; (3) AS-I4/5/6 autostart negatives — a bad project on a line must
degrade safely; (4) EX-N2/N3 attach guard; (5) extract the crash parser +
CR-U*; (6) FE-E4/E5 timing paths.
