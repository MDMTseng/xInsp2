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

Master + spike branches share the layout. Tests on the
`shm-process-isolation` spike branch are listed separately below.

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

All exit with `ALL TESTS PASSED`.

---

## Node integration suites (`node --test`)

Run all: `cd vscode-extension && node --test test/*.test.mjs`

| Suite | Coverage |
|---|---|
| `protocol.test.mjs` | TS protocol mirror parses C++ fixtures |
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

## Spike-only tests (`shm-process-isolation` branch)

| Binary | Coverage |
|---|---|
| `test_xi_shm` | cross-process SHM region: parent+child attach, byte-match, refcount |
| `test_worker` | `xinsp-worker.exe` end-to-end with test_doubler plugin |
| `test_isolated_instance` | `open_project` with `isolation:"process"` opt-in |
| `test_worker_respawn` | auto-respawn on worker death + rate-limit cap |
| `test_worker_timeout` | per-call `CancelIoEx` watchdog + dead state |
| `test_script_runner` | `xinsp-script-runner.exe` + bidirectional RPC |
| `test_script_process_adapter` | `ScriptProcessAdapter` lifecycle + handler |
| `test_script_runner_respawn` | script runner auto-respawn |
| `test_shm_ipc_edges` | DoS hardening, OOM, bogus handles, attach errors |

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

- **Plugin / script crashes still kill the backend** on the default
  in-proc path. Process isolation is shipped but opt-in:
  `instance.json: "isolation": "process"` for plugins,
  `cmd:script_isolated_run` for scripts. Default-on is tracked work.
  See `docs/reference/ipc-shm.md`.
- **Linux** build path untested (Windows-first WS server, SEH usage,
  `cl.exe` compile driver).
- **Multi-client server** deliberately deferred to S6.
- **Linux** build path untested (Windows-first WS server, SEH usage,
  `cl.exe` compile driver).
- **Multi-client server** deliberately deferred to S6.

---

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
