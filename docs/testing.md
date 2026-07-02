# Testing

This is the live picture of test surface, organisation, and how to run.
**Update on every test addition.** Replaces the older `TEST_PLAN.md` /
`TEST_REPORT.md` / `TestAudit.md` trio.

> **Note.** The VAR value-tracking, the
> `vars` wire message, the core binary image-preview frame, and the core
> `subscribe`/`unsubscribe` commands have been removed from the backend. Script
> data-out now goes through the shipped `expose` plugin (per-channel subscription
> over `exchange` + one atomic `XEX1` frame per record). Suites that
> exercised the old core paths (e.g. `ws_run_vars`, `ws_preview`, `runSubscribe`, the
> JPEG-preview assertions in `ws_comprehensive`, the preview-header rows in
> `test_protocol`, the *JPEG encode* note below) cover **removed** behavior and
> are legacy — to be retired or rewritten. They are listed below
> as they stand today; treat the old preview/vars/subscribe coverage as legacy.

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
| `test_record` | yyjson-backed `Record`; path expressions (`a.b[0].c`), image bag, default returns; γ-4 refcount + COW, cross-ABI share/adopt, zero-copy input cache |
| `test_protocol` | `parse_cmd`, `Rsp` / `VarItem` / preview header serialization; fixture parity with TS side |
| `test_image_pool` | 16-shard refcounted pool; concurrent create/release/data; 20 MP allocation |
| `test_image_pool_stress` | high-volume concurrent pool churn; refcount integrity under load |
| `test_sha256` | SHA-256 digest vectors (WS-auth bearer hashing) |
| `test_diagnostics` | cl.exe / link.exe diagnostic parser (error / warning / fatal / note shapes) |
| `test_qa_fault` / `test_qa_race` | FE respawn sliding-window + cap arithmetic; forensics carried on the cap event |
| `test_qa_edge` | FE crash-report parser (`xi_crash_report.hpp`): good / threads[] fallback / no-minidump / corrupt / last-wins, against crafted fixtures; crash-history JSONL builder; FE status renderer (`fe_status`): healthy/latched snapshots, last_event forensics, path escaping |

All exit with `ALL TESTS PASSED`.

**Non-C++ gates in the same ctest run:**
- `doc_coverage` (`tools/check_doc_coverage.py`) — a freeze guard for
  developer-facing docs. Derives the public surface (plugin/script C-ABI exports
  the host resolves, the authoring `XI_*` macros, and the WS commands) from the
  source of truth and **fails** if any item is undocumented in `docs/guides` or
  `docs/reference` (internal residue + `docs/archive` deliberately don't count) and
  not listed in `docs/.doc-coverage-allow` with a reason. This is what stops a new
  export/command/macro from shipping while the guides lag. Run standalone:
  `python tools/check_doc_coverage.py`. Skipped only if no Python3 is found.
- `perf_*` — **best-case micro-throughput regression gates** (see *Performance
  baseline* below). Each metric is a best-of/minimum over repeated batches, so it
  catches a same-machine, same-backend slowdown in a hot inner op; it does **not**
  prove p99/tail or end-to-end latency. Each baseline carries an environment
  fingerprint (`xi.perf-baseline/1`): on a different CPU / JPEG backend / build
  type — or against a pre-fingerprint baseline — the gate **skips with a reason**
  rather than reporting a false regression.
- `script_selfcheck` — a compile-only guard that `xi_script_support.hpp` (force-
  included into every inspection script) stays **self-sufficient**. It compiles a
  TU that includes *only* that header; if the header ever needs a symbol it doesn't
  directly `#include` (e.g. relying on a transitive include a refactor later
  severs — as happened once with `xi::InstanceRegistry`), this goes red. The
  backend's own TUs don't catch it (they pull the header in from elsewhere), so
  this is the only automated check of the bare-script compile path.

---

## Node integration suites (`node --test`)

Run all: `cd vscode-extension && node --test test/*.test.mjs`

| Suite | Coverage |
|---|---|
| `protocol.test.mjs` | TS protocol mirror parses C++ fixtures |
| `backend_mode.test.mjs` | `resolveBackendMode` managed/attach/auto decision (FE/BE ownership) |
| `ws_basic.test.mjs` | ping / version / hello / shutdown / unknown |
| `ws_buffer_replay.test.mjs` | replay as plugin composition: the `cache` plugin (instance `buffer` in `buffer_replay_demo`) captures + re-emits records (host no longer owns record/replay) |
| `ws_ordered_sink.test.mjs` | ordered output sink: `"sink": true` plugin (builds `sdk/examples/comm`) receives `use(sink).process()` in frame-arrival order under `dispatch_threads=4`, host-stamped `$seq` strictly increasing |
| `ws_expose_sink.test.mjs` | `expose` plugin: `xi::use("expose").process(rec)` (record tagged with `"$channel"`) → one atomic `XEX1` `emit_binary` frame per subscribed channel + `subscribe`/`unsubscribe`/`get`/`list_channels` exchange commands, content-hash image dedup |
| `ws_run_vars.test.mjs` | *(legacy — removed behavior)* run → vars round-trip with image gid |
| `ws_preview.test.mjs` | *(legacy — removed behavior)* binary preview frame format |
| `ws_trigger.test.mjs` | compile + start continuous mode → multiple runs fire |
| `ws_state.test.mjs` | `xi::state` persists across reload |
| `ws_compile_reload.test.mjs` | compile_and_load + version increment |
| `ws_crash.test.mjs` | null deref / div0 / array overrun → backend survives |
| `ws_plugins.test.mjs` | plugin scan + create_instance |
| `ws_fallback_gate.test.mjs` | γ-4 load gate: yyjson-layout-incompatible plugin (no `xi_yyjson_abi`) refused at load; `json_fallback:true` opt-in loads it |
| `ws_cache_input.test.mjs` | γ-4 v4-4 end-to-end: a real plugin caches its input doc across frames (zero-copy); frame N reads frame N-1's input back |
| `ws_project.test.mjs` | save_project / load_project / open_project |
| `ws_defect.test.mjs` | `defect_detection.cpp` end-to-end |
| `ws_reload_verify.test.mjs` | hot-reload preserves state, params, fresh code |
| `ws_adversarial.test.mjs` | malformed JSON, huge payload, rapid-fire, path injection, double-start |
| `ws_commands.test.mjs` | start/stop, set_param, exchange_instance, etc. |
| `ws_comprehensive.test.mjs` | compile fail, value verification, JPEG preview, project open |
| `ws_fixturing.test.mjs` | test-fixture helpers and backend-lifecycle helpers |
| `ws_graph_capture.test.mjs` | dataflow edge capture (headless half of graph stage-2) |
| `ws_io_field.test.mjs` | typed I/O field extraction and NA propagation per-field |
| `ws_io_mutate.test.mjs` | typed I/O mutation: `set_param` + `exchange_instance` wiring |
| `ws_io_stress.test.mjs` | high-volume typed I/O round-trips under concurrency |
| `ws_multi_file_script.test.mjs` | multi-file script headers (`#include`d `.cpp` sources) |
| `ws_na_propagation.test.mjs` | NA backbone: NA input → NA output propagation at plugin boundary |
| `ws_region.test.mjs` | region-typed I/O (ROI, mask, polygon) |
| `ws_types.test.mjs` | nominal type system: named I/O types, type-compatibility checks |
| `ws_types_cv.test.mjs` | OpenCV-typed I/O (cv::Mat wrapper types) |

---

## E2E runners (`vscode-extension/test/run*.mjs`)

Each launcher spawns a real backend, drives WS commands, asserts on disk
and protocol artifacts. Run individually, e.g.:

```
cd vscode-extension && node test/runUserJourney.mjs
```

| Runner | What it proves |
|---|---|
| `runMulticam` | `synced_stereo` gathering plugin emits one record carrying left+right (same `seq`) |
| `runSubscribe` | *(legacy — removed behavior)* preview subscription gates binary frames by name |
| `runWatchdog` | watchdog kills runaway inspect; backend stays alive |
| `runRemoteAuth` | `--auth` bearer gate, 401 on bad/missing, constant-time compare |
| `runHeadlessRunner` | `xinsp-runner.exe` produces JSON report from a project |
| `runUserJourney` | full 10-step real-user flow (24 screenshots) |
| `runProjectPluginJourney` | in-project plugin create / edit / typo / fix / instance / export (12 screenshots) |
| `runImageViewerJourney` | plugin + interactive image viewer pan/zoom/fit/1:1/tool ops (18 screenshots, scripted via `xinsp2.imageViewer.applyOp`) |
| `runUxStates` | UX state transitions (welcome → project → instance) |
| `runCrashDump` | backend crash → minidump + crash report survive with correct breadcrumbs |

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
| `hover_contract.cjs` | `runHoverContract.mjs` |
| `webui_screenshot.cjs` | `runWebuiScreenshot.mjs` |
| `pipeline_graph.cjs` | `runPipelineGraph.mjs` |
| `graph_capture.cjs` | `runGraphCapture.mjs` |
| `graph_multi.cjs` | `runGraphMulti.mjs` |
| `show_icon.cjs` | asserts the activity-bar icon renders correctly (screenshot) |
| `viewer_run.cjs` | opens the viewer panel and asserts the rendered layout |
| `journey_helpers.cjs` | shared utilities (`editAndSave`, `makeShooter`, etc.) |

`hover_contract.cjs` opens `examples/blob_tracker`, lets the managed
backend connect, and asserts that hovering `xi::use("det")` resolves to the
plugin's copyable Inputs/Outputs/Params contract (read-only — it never
recompiles the plugin, so it can run alongside a backend that already has
`blob_centroid_detector.dll` loaded).

`webui_screenshot.cjs` opens the same project, runs `xinsp2.openInstanceUI`
for the `det` instance, asserts the webview panel tab actually opened (via
`vscode.window.tabGroups`), and PrintWindow's the dev-host into
`screenshot/webui_*.png` for a human spot-check. Also read-only.

`pipeline_graph.cjs` covers the stage-1 pipeline graph: asserts
`extractPipelineNodes()` finds the script's `xi::use("…")` instances (with
plugin + manifest I/O counts), opens the `xinsp2.openPipelineGraph` webview,
and screenshots it. Nodes only — clicking a node opens its webui.

`pipeline_graph.cjs` also covers the VAR "script-glue" chips interleaved with
plugin nodes by source order, and the capture button's sample-frame auto-pick
(`<project>/frames`) so a frame-driven project's capture actually runs.

`graph_capture.cjs` covers the stage-2 dataflow edges: builds a temp project
with two chained `blob_centroid_detector` instances, captures one run with
dataflow recording on (`captureGraphEdges`), and asserts the reconstructed
a→b edge (via the `cleaned` image) plus that `renderPipelineGraphHtml` emits
the SVG connector. The backend half is also covered headless by
`ws_graph_capture.test.mjs`. Capture is OFF by default (no hot-path cost).

`graph_multi.cjs` is a 3-node connected pipeline (a→b→c, chained by the
`cleaned` image): it captures, repaints the open graph panel with edges
(`captureAndRenderGraph`), and screenshots the connection lines.

---

## Performance baseline (1920×1080 RGB)

Numbers from a recent sweep. Drift if you change ops / encoders. These are
**best-case micro-throughput** figures (best-of/minimum timing) — a floor for a
hot op on the reference machine, not a p99/tail or end-to-end latency budget.

The committed perf-gate baselines (`backend/tests/perf_baselines/*.txt`) each
carry an `xi.perf-baseline/1` environment fingerprint (cpu, logical_cores, os,
build_type, opencv, jpeg_backend, xinsp commit). The JPEG metric key is
**backend-aware** (`jpeg_<backend>_q85_1920x1080_us`) because turbojpeg / OpenCV /
IPP / stb differ by 2–5×, so a machine running a different encoder is not a
regression. A run whose fingerprint doesn't match the baseline (different
hardware/backend/build, or an old baseline with no fingerprint) is **skipped
with a reason**, not failed. Re-capture on a machine with
`-DUPDATE_BASELINE=ON` (see `backend/tests/perf_gate.cmake`).

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

### What these numbers do and don't claim

Precise wording so the baselines aren't over-read:

- **Dispatch groups give worker-capacity isolation, not full isolation.** A
  group bounds how many workers a source class can occupy (its lane / `max_parallel`),
  so one source can't starve another of *dispatch slots*. Groups still **share**
  CPU cores, memory bandwidth, the allocator + `ImagePool`, the OpenCV/OpenMP
  thread pools, any shared non-reentrant plugin instance, I/O, and the
  same-process failure domain (a crash in one takes the process down). Treat a
  group as a capacity lane, not a sandbox.
- **Image lifecycle is zero-copy across plugin boundaries, not zero-allocation.**
  Records and images pass **by pointer / refcounted handle** across in-process
  plugin boundaries (no serialize, no pixel copy — see `test_record`,
  `ws_cache_input`), with bounded, refcounted ownership. But `ImagePool::create`
  currently **allocates** a fresh pixel buffer and memsets it (first-touch page
  faults), and `release` frees it — so output allocation is **per-image unless a
  buffer is reused**. The measured cost (`bench_image_pool`) is small enough for
  target rates (its header explains why buffer reuse isn't worth it) — but this is
  *bounded, acceptable* allocation, **not** "zero allocation per frame".
- **The runtime timer / synthetic-source cadence is soft (best-effort), not
  deadline-accurate.** The timer source and `set_timer_fps` path schedule via
  **relative sleeps**, so the effective rate drifts under load and is not a hard
  real-time deadline. Use it as a best-effort cadence source, not a guaranteed
  frame clock.

---

## Known limitations / gaps

- **Plugin / script crashes kill the backend.** Process isolation +
  SHM were removed 2026-05; all plugins and scripts run in-process.
  The replacement safety net is crash diagnosability (minidumps +
  per-thread breadcrumbs + PDB symbolication, see
  `docs/guides/debug.md`). `docs/archive/ipc-shm.md` documents
  the removed mesh for historical reference only.
  `examples/plugin_crash_forensics/` is the regression that proves the
  net: it arms a plugin to crash the backend from a raw (unmanaged)
  thread, then asserts the minidump + crash report survive with the
  dispatch thread's `last_phase="inspect"` breadcrumb intact.
  `examples/fe_supervisor/` is the next layer up: it runs the
  `xinsp-fe.exe` supervisor against an auto-crashing project and asserts
  the FE detects each death, records its crash forensics (from the backend
  log), respawns rate-limited, latches `down` at the cap, and leaves no
  orphaned backend. It also asserts the FE's
  **crash-history** JSONL (`xi_crash_history.hpp`): one record per death,
  `consecutive` counting up 1..N, the final death marked `cap_hit=true`,
  forensics + minidump path on each; and the FE **status channel**
  (`xi_fe_status.hpp` → `fe-status.json`): a latched `down` state carrying the
  death reason + forensics, the value a UI reads instead of inferring it
  from a WS disconnect. (Windows-only; skips on non-`nt`.)
- **PLC comms as a plugin** (the `xinsp-comms` gateway + `xi::comms` were removed;
  see `docs/archive/comms-gateway.md`). Line-safe-on-crash is no longer brokered by
  the core or the FE: a comms plugin spawns its **own sidecar process** that holds
  the PLC link, watches the backend process handle, and signals the PLC to go
  line-safe when the backend dies (the `set_safe_state` ABI verb + the FE
  PLC-delivery sink were removed 2026-06).
  `examples/script_external_dll/` — a user **script** using an external DLL:
  proves `project.json` `include_dirs` + `link_libs` feed the script compile and
  that the dependency DLL is found at runtime from the project folder (on the DLL
  search path). Builds a tiny `extmath.dll`, compiles+loads the script, asserts
  `ext_add(2,3)==5`.
- **Phase G stress + race** (#92; see
  `docs/archive/fe-be-split-test-plan.md` "Phase G"). Beyond the `test_qa_fault` /
  `test_qa_race` units above: `examples/qa_recover/` proves the **recover-and-clear** transition
  (a backend that crashes a few times then heals → FE clears `down` back to
  `healthy`, never hits the cap — the `crash_then_heal` plugin counts crashes in a
  respawn-surviving marker). (The `qa_soak` full-stack FE+BE+gateway soak was
  retired with the comms gateway.)
- **Burst-parallelism safety** — `backend/tests/test_set_def_race.cpp` proves the
  declared-reentrancy model through the real CallScope admission gate: a
  **non-reentrant** instance (cap=1) serializes all entry points, so a `set_def`
  can never race an in-flight `process()` on the same instance (0 torn reads),
  while a **`reentrant: true`** one runs ungated (races — the plugin must guard
  itself). `backend/tests/test_prepare_concurrency.cpp` adds the ABI v7 staging
  contract (`prepare` ungated, `commit` gated). Guards against a regression that
  lets a `set_def` into a non-reentrant instance while `process()` is in flight.
  `examples/qa_reentrancy/` demonstrates the same model end-to-end (`python
  driver.py` → VERDICT: PASS) — a 4-thread pool pokes serial / parallel / capped
  probe instances and asserts max concurrency 1 / ≥2 / 1.
- **Plugin dependency-DLL base-name clash** — `examples/dll_version_clash/`
  (`python run_experiment.py`) builds two versions of a same-named dependency and
  proves the Windows loader's one-module-per-base-name rule across three load
  modes (full-path → no clash, by-name same-name → clash, by-name distinct → no
  clash).
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
  `docs/guides/deploy.md`. Windows-only; skip on non-`nt`.
- **Result ordering** — `examples/qa_result_order/` proves
  `parallelism.result_order: "arrival"`. The same uneven-timing script runs
  under both modes with `dispatch_threads=4`: **completion** reorders (out-of-order
  `run_id`s on the wire), **arrival** emits in frame-arrival order (zero
  inversions) while compute still runs parallel. Windows-only; skip on non-`nt`.
- **Dispatch groups (per-group lanes)** — Windows-only regressions for the
  `parallelism.groups` model (see `docs/internals/dispatch.md` cheat-sheet):
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
