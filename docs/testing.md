# Testing

This is the live picture of test surface, organisation, and how to run.
**Update on every test addition.** Replaces the older `TEST_PLAN.md` /
`TEST_REPORT.md` / `TestAudit.md` trio.

---

## Where the tests live

```
backend/tests/         ← C++ unit + integration tests (ctest)
vscode-extension/test/
  ├── *.test.mjs        ← Node integration suites (`node --test`)
  ├── run*.mjs          ← E2E launchers (drive a real backend)
  └── e2e/              ← VS Code Extension Host suites (@vscode/test-electron)
```

---

## C++ unit tests (master)

Run all: `ctest --test-dir backend/build -C Release`

| Binary | Coverage |
|---|---|
| `test_xi_core` | `xi::async`, `xi::Var`, `xi::Param`, `xi::Instance` registries; `await_all`, `Future<void>`, ASYNC_WRAP |
| `test_record` | cJSON-backed `Record`; path expressions (`a.b[0].c`), image bag, default returns |
| `test_protocol` | `parse_cmd`, `Rsp` / `VarItem` / preview header serialization; fixture parity with TS side |
| `test_image_pool` | 16-shard refcounted pool; concurrent create/release/data; 20 MP allocation |
| `test_ops` | `toGray` / `threshold` / `boxBlur` / `gaussian` / `sobel` / open / close / `adaptiveThreshold` / `canny` / `findContours` (boundary) / `findFilledRegions` / `matchTemplateSSD` / stats |
| `test_diagnostics` | cl.exe / link.exe diagnostic parser (error / warning / fatal / note shapes) |
| `test_safe_state` | FE `SafeStateSink`: reason→string, factory fallthrough, log formatting + `ts=`, empty-field placeholders, overflow/null safety |
| `test_qa_fault` / `test_qa_race` | FE respawn sliding-window + cap arithmetic; forensics carried on the cap event |
| `test_qa_edge` | FE crash-report parser (`xi_crash_report.hpp`): good / threads[] fallback / no-minidump / corrupt / last-wins, against crafted fixtures; crash-history JSONL builder (`xi_crash_history.hpp` CH-U*): field coverage, Windows-path backslash escaping, control-char escaping, empty-event totality; FE status renderer (`xi_fe_status.hpp` FS-U*): healthy/latched snapshots, comms object, last_event forensics, path escaping |
| `test_qa_stress` | Phase G stress/fuzz of the respawn-cap + safe-state core: degenerate caps, exact reset boundary, recover-then-recur, a 200k-step equivalence fuzz vs a reference model, high-volume bounded emission |

All exit with `ALL TESTS PASSED`.

---

## Node integration suites (`node --test`)

Run all: `cd vscode-extension && node --test test/*.test.mjs`

| Suite | Coverage |
|---|---|
| `protocol.test.mjs` | TS protocol mirror parses C++ fixtures |
| `backend_mode.test.mjs` | `resolveBackendMode` managed/attach/auto decision (FE/BE ownership) |
| `ws_basic.test.mjs` | ping / version / hello / shutdown / unknown |
| `ws_run_vars.test.mjs` | run → vars round-trip with image gid |
| `ws_preview.test.mjs` | binary preview frame format |
| `ws_trigger.test.mjs` | TriggerBus emit_trigger + sink |
| `ws_state.test.mjs` | `xi::state` persists across reload |
| `ws_compile_reload.test.mjs` | compile_and_load + version increment |
| `ws_crash.test.mjs` | null deref / div0 / array overrun → backend survives |
| `ws_plugins.test.mjs` | plugin scan + create_instance |
| `ws_project.test.mjs` | save_project / load_project / open_project |
| `ws_defect.test.mjs` | `defect_detection.cpp` end-to-end |
| `ws_reload_verify.test.mjs` | hot-reload preserves state, params, fresh code |
| `ws_adversarial.test.mjs` | malformed JSON, huge payload, rapid-fire, path injection, double-start |
| `ws_commands.test.mjs` | start/stop, set_param, exchange_instance, etc. |
| `ws_comprehensive.test.mjs` | compile fail, value verification, JPEG preview, project open |

---

## E2E runners (`vscode-extension/test/run*.mjs`)

Each launcher spawns a real backend, drives WS commands, asserts on disk
and protocol artifacts. Run individually, e.g.:

```
cd vscode-extension && node test/runUserJourney.mjs
```

| Runner | What it proves |
|---|---|
| `runMulticam` | TriggerBus pairs left+right under same tid (`synced_stereo`) |
| `runSubscribe` | preview subscription gates binary frames by name |
| `runVariants` | compare_variants applies A → run → snapshot → B → run → snapshot |
| `runBreakpoint` | `xi::breakpoint("label")` parks worker; `cmd:resume` releases |
| `runWatchdog` | watchdog kills runaway inspect; backend stays alive |
| `runRemoteAuth` | `--auth` bearer gate, 401 on bad/missing, constant-time compare |
| `runTriggerPolicies` | Any / AllRequired / LeaderFollowers all behave correctly |
| `runHistory` | ring buffer keeps 50; `since_run_id` filter; `set_history_depth` resize |
| `runHeadlessRunner` | `xinsp-runner.exe` produces JSON report from a project |
| `runRecordReplay` | record observer-mode → replay through bus → events match |
| `runUserJourney` | full 10-step real-user flow (24 screenshots) |
| `runProjectPluginJourney` | in-project plugin create / edit / typo / fix / instance / export (12 screenshots) |
| `runImageViewerJourney` | plugin + interactive image viewer pan/zoom/fit/1:1/tool ops (18 screenshots, scripted via `xinsp2.imageViewer.applyOp`) |
| `runUxStates` | UX state transitions (welcome → project → instance) |

---

## VS Code Extension Host suites

Located at `vscode-extension/test/e2e/`. Launched by the matching
`run*.mjs`. Each suite drives the editor via `vscode.commands.executeCommand`
and asserts on tab state, tree contents, and disk artifacts. Suites
capture screenshots via `Win32 PrintWindow` for human spot-checks.

| Suite | Launcher |
|---|---|
| `index.cjs` | dispatches by `XINSP2_E2E_SUITE` env |
| `user_journey.cjs` | `runUserJourney.mjs` |
| `project_plugin_journey.cjs` | `runProjectPluginJourney.mjs` |
| `image_viewer_journey.cjs` | `runImageViewerJourney.mjs` |
| `full_pipeline.cjs` | `runE2E.mjs` |
| `json_source_ui.cjs` | `runJsonSourceUI.mjs` |
| `ux_states.cjs` | `runUxStates.mjs` |
| `journey_helpers.cjs` | shared utilities (`editAndSave`, `makeShooter`, etc.) |

---

## Performance baseline (1920×1080 RGB)

Numbers from a recent sweep. Drift if you change ops / encoders.

### JPEG encode (q=85)

| backend | per-encode | throughput |
|---|---:|---:|
| stb (no SIMD) | 17.30 ms | 120 MP/s |
| OpenCV imencode | 16.36 ms | 127 MP/s |
| **libjpeg-turbo** | **2.71 ms** | **765 MP/s** |

### Image ops (1920×1080)

| op | C++ only | OpenCV | **IPP** |
|---|---:|---:|---:|
| toGray (RGB→Gray) | 1.89 ms | 0.89 ms | **0.73 ms** |
| threshold | 1.09 ms | 0.59 ms | 0.68 ms |
| gaussian(r=3) | 26.83 ms | **1.16 ms** | 2.73 ms |
| sobel | 5.02 ms | 8.03 ms | 5.26 ms |
| erode(r=1) | 24.66 ms | 0.63 ms | **0.55 ms** |
| dilate(r=1) | 24.64 ms | 0.63 ms | **0.54 ms** |

Dispatch order: **IPP → OpenCV → portable C++** (selected at compile).

---

## Known limitations / gaps

- **Plugin / script crashes kill the backend.** Process isolation +
  SHM were removed 2026-05; all plugins and scripts run in-process.
  The replacement safety net is crash diagnosability (minidumps +
  per-thread breadcrumbs + PDB symbolication, see
  `docs/guides/debugging.md`). `docs/reference/ipc-shm.md` documents
  the removed mesh for historical reference only.
  `examples/plugin_crash_forensics/` is the regression that proves the
  net: it arms a plugin to crash the backend from a raw (unmanaged)
  thread, then asserts the minidump + crash report survive with the
  dispatch thread's `last_phase="inspect"` breadcrumb intact.
  `examples/fe_supervisor/` is the next layer up: it runs the
  `xinsp-fe.exe` supervisor against an auto-crashing project and asserts
  the FE detects each death, drives the line to a safe state (with crash
  forensics from the backend log), respawns rate-limited, hits the cap,
  stays safe, and leaves no orphaned backend. It also asserts the FE's
  **crash-history** JSONL (`xi_crash_history.hpp`): one record per death,
  `consecutive` counting up 1..N, the final death marked `cap_hit=true`,
  forensics + minidump path on each; and the FE **status channel**
  (`xi_fe_status.hpp` → `fe-status.json`): a latched safe state carrying the
  death reason + forensics, the value a UI reads instead of inferring "down"
  from a WS disconnect. (Windows-only; skips on non-`nt`.)
- **Comms gateway** (out-of-process PLC I/O; see
  `docs/design/comms-gateway.md`). Three regressions, Windows-only:
  `examples/comms_gateway/` — the `xinsp-comms` relay round-trip + dead-man
  (backend crash → gateway fires the registered emergency payload to the PLC);
  `examples/comms_script/` — the backend-side `xi::comms` client end-to-end
  (script ↔ PLC sim through the gateway); `examples/fe_comms/` — `xinsp-fe.exe`
  supervising the gateway as a sibling of the backend: asserts the script
  reaches the PLC, then kills the gateway and asserts the FE drives
  `CommsLost` safe-state, respawns it, the backend survives, the link is
  restored, and nothing orphans.
  `examples/dll_version_clash/` — two plugins each depending on a different
  version of a same-named DLL, in one process. Proves the loader rules: a
  **by-name** (static-import) dependency collides on base name (second plugin
  silently gets the first's version), an absolute-path load stays distinct, and
  distinct file names are the fix. Builds the two dep versions with `cl` (vcvars
  located via `toolchain_health`) and asserts all three outcomes.
  `examples/script_external_dll/` — a user **script** using an external DLL:
  proves `project.json` `include_dirs` + `link_libs` feed the script compile and
  that the dependency DLL is found at runtime from the project folder (on the DLL
  search path). Builds a tiny `extmath.dll`, compiles+loads the script, asserts
  `ext_add(2,3)==5`.
- **Phase G stress + race** (#92; see
  `docs/design/fe-be-split-test-plan.md` "Phase G"). Beyond the `test_qa_stress`
  unit above: `examples/qa_recover/` proves the **recover-and-clear** transition
  (a backend that crashes a few times then heals → FE `CLEAR SAFE STATE`, never
  hits the cap — the `crash_then_heal` plugin counts crashes in a
  respawn-surviving marker), and `examples/qa_soak/` is a **healthy full-stack
  soak** (FE+BE+gateway): sustained normal operation trips no false safe-state,
  respawns nothing, keeps the heartbeat advancing and the PLC link up, then shuts
  down clean. Windows-only; skip on non-`nt`.
- **Burst-parallelism safety** — `examples/qa_reentrancy/` proves the
  declared-reentrancy model for the parallel dispatch pool
  (`parallelism.dispatch_threads > 1`). Under a 4-thread pool it pokes two probe
  instances every frame and asserts the **non-reentrant** one stays serialized
  (max concurrency 1, zero overlaps — the host's per-instance lock holds) while
  the **`reentrant: true`** one runs concurrently (max concurrency ≥ 2), and a
  third reentrant instance with `instance.json` **`max_concurrency: 1`** is held
  back to 1 (per-instance concurrency cap). Guards against a regression that lets
  two workers into a non-reentrant (or capped) instance at once. Windows-only
  (plugin compile); skip on non-`nt`.
- **Per-worker watchdog** — `examples/qa_watchdog/` proves the inspect watchdog
  now fires under `dispatch_threads > 1` (it tracks a deadline slot per worker,
  not one global slot) and that a hard trip (script ignores cooperative cancel)
  makes the backend **exit** with `0x5744` for the FE to respawn — rather than
  `TerminateThread` a worker (which would leak the per-instance lock). The N=1
  path + WS contract is covered by `vscode-extension/test/runWatchdog.mjs`.
  Windows-only; skip on non-`nt`.
- **Project working copy** — `examples/qa_working_copy/` proves the
  transactional `<project>/.xinsp_work` scratch (`open_project working_copy:true`
  + `commit_working_copy` / `discard_working_copy`). Drives the full lifecycle on
  an on-disk `kv` instance: seed → edits isolated to the scratch (canonical
  untouched) → **crash** (hard-kill) + restart resumes the scratch (uncommitted
  edits survive) → commit pushes to the canonical project → discard reverts to
  the committed state. `driver_fe.py` adds the FE layer: the supervisor forwards
  `--working-copy`, and a hard-kill of the backend → FE respawn resumes the
  scratch with the uncommitted edit intact. See
  `docs/guides/project-working-copy.md`. Windows-only; skip on non-`nt`.
- **Result ordering** — `examples/qa_result_order/` proves
  `parallelism.result_order: "arrival"`. The same uneven-timing script runs
  under both modes with `dispatch_threads=4`: **completion** reorders (out-of-order
  `run_id`s on the wire), **arrival** emits in frame-arrival order (zero
  inversions) while compute still runs parallel. Windows-only; skip on non-`nt`.
- **Dispatch groups (per-group lanes)** — Windows-only regressions for the
  `parallelism.groups` model (see `docs/design/dispatch-groups.md` cheat-sheet):
  `qa_dispatch_groups` (gating + clamp + warnings), `qa_two_group_paths` (two
  sources route to two lanes, zero cross-routing), `qa_group_parallelism` (peak
  running == `max_parallel`, 1/2/4, + arrival ordering), `qa_group_stress` (8
  groups × 4 workers, 20/s burst, near-saturation), `qa_cpu_affinity` (a bound
  group only runs on its core mask), `qa_min_interval` (rate cap 20/s→10/s),
  `qa_runtime_settings` (live `set_timer_fps`/`set_process_priority` + the
  `project.json` `runtime` block applied on open).
- **Per-run Result** — `examples/qa_run_result/` proves the `run_result` event
  (verdict code + message, dropped→`XI_SYS_DROPPED`, reserved-band warning).
- **local_image_source auto mode** — `examples/qa_local_auto/` proves the reused
  "local" source self-emitting a folder on a timer (auto-update).
- **BE-served dashboard** — `examples/qa_get_dashboard/` proves `cmd:get_dashboard`
  serves `<project>/dashboard[.<name>].json` (missing → found:false; path traversal
  blocked) so the HMI needs only the WS URL.
- **Export bundle (AOT, no toolchain)** — `examples/qa_export_bundle/` exports a
  project via `tools/export_bundle.py`, then runs the bundle backend with `--aot`
  and a **stripped PATH** (no `cl.exe`) and still inspects — proving prebuilt
  plugin/script DLLs load with no compiler on the target.
- **ImagePool throughput** — `backend/tests/bench_image_pool` (manual, not a
  ctest): create/release cost; its header records why buffer reuse isn't worth it.
- **Linux** build path untested (Windows-first WS server, SEH usage,
  `cl.exe` compile driver).
- **Multi-client server** deliberately deferred to S6.

---

## Running the `examples/qa_*` suite

`python tools/run_qa.py [filter]` runs every `examples/qa_*/driver.py`
sequentially, aggregates `VERDICT: PASS|FAIL|SKIP`, and exits non-zero on any
failure (CI-gate usable). `python tools/run_qa.py group` runs only the
folder-name matches; `--list` lists without running; `--timeout=N` caps each.
Tests run one at a time (each spawns its own backend on its own port and some
share project-plugin DLL paths). Windows-first; drivers SKIP on non-`nt`.

## How to add a new test

1. **C++ unit** → `backend/tests/test_<name>.cpp` + add to
   `backend/CMakeLists.txt` (model after the closest existing entry).
   `add_test(NAME <name> COMMAND test_<name>)` registers it for ctest.
2. **Node integration** → drop a new `*.test.mjs` under
   `vscode-extension/test/`. Use the existing
   `helpers/<helpers files>` for spawning a backend.
3. **E2E** → add a `cjs` under `vscode-extension/test/e2e/`, dispatch
   from `e2e/index.cjs`, write a `runFoo.mjs` launcher. Use
   `journey_helpers.cjs` for screenshot + `editAndSave` patterns.
