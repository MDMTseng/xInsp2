# xInsp2 — Status & Roadmap

What's shipping, what's in flight, what's planned.
**Update this whenever a milestone closes.** Single source of truth — no
parallel `DEV_PLAN.md` / `STATUS.md` to drift against.

> Adjacent reading: [`architecture.md`](./architecture.md) for the
> technical map, [`testing.md`](./testing.md) for the test surface,
> [`protocol.md`](./protocol.md) for the WS reference.

---

## TL;DR

xInsp2 ships as a single-machine inspection-authoring environment:

- **Backend** (`xinsp-backend.exe`) — WebSocket service, plugin loader,
  script compiler driver, image pool, recording / replay.
- **VS Code extension** — primary UI: tree views, viewer panel, plugin
  webviews, project / plugin / instance management, in-project plugin
  authoring, interactive image viewer with pan / zoom + pick tools.
- **Headless runner** (`xinsp-runner.exe`) — same backend stack minus
  the WebSocket; suitable for production deployments.
- **SDK** — scaffold + cmake helpers + tests for plugin authors who
  want to ship distributable plugins.

Master holds **7 shipped plugins** under `plugins/`. Process isolation +
the SHM mesh were **removed 2026-05** in favour of a single in-process
compute core (BE) under a frontend (FE) supervisor — all plugins
(cameras included) run in-process, zero-copy via pointers. See
*Process isolation* below.

---

## Shipping today (master)

### Core platform

| Layer | Status |
|---|---|
| Stable C ABI for plugins (`xi_abi.h` / `XI_PLUGIN_IMPL`) | ✅ |
| Sharded refcounted ImagePool (16 shards, atomic refcount) | ✅ |
| SEH crash isolation on every script + plugin call site | ✅ |
| `xi_seh.hpp` + `xi::spawn_worker` (translator on plugin worker threads) | ✅ |
| Auto-respawn of crashed backend (extension watches process) | ✅ |
| Crash-safe atomic JSON writes (`xi_atomic_io.hpp`) | ✅ |
| Skip-bad-instance on `open_project` (one bad instance ≠ broken project) | ✅ |
| Compile diagnostics → VS Code Problems panel (squiggles on save) | ✅ |
| Per-instance folder (`<project>/instances/<name>/`) | ✅ |

### Inspection authoring

| Feature | Status |
|---|---|
| `Instance<T>` / `Param<T>` / `xi::use("name")` | ✅ |
| `xi::Record` (cJSON-backed, path expressions, image bag) | ✅ |
| `xi::Json` (RAII JSON builder + reader) | ✅ |
| `xi::state()` persistent cross-frame / cross-reload | ✅ |
| `xi::breakpoint(label)` + `cmd:resume` | ✅ |
| `xi::async` / `Future<T>` parallel ops (SEH-safe) | ✅ |
| Auto-compile-on-save with hot reload | ✅ |
| Script DLL versioning (`stem_vN.dll`) for Windows lock survival | ✅ |

### Plugin development

| Feature | Status |
|---|---|
| **In-project plugins** (`xinsp2.createProjectPlugin`) — Easy / Medium / Expert templates | ✅ |
| Hot reload on plugin source save (state preserved across rebuild) | ✅ |
| Export project plugin (Release + cert + standalone folder) | ✅ |
| Standalone SDK scaffold (`sdk/scaffold.mjs` + `xinsp2_add_plugin`) | ✅ |
| Per-plugin baseline cert system (`cert.json`) | ✅ |
| Plugin webview panel with auto-rendered def schema | ✅ |

### Multi-camera / triggering

| Feature | Status |
|---|---|
| TriggerBus + 3 policies (Any / AllRequired / LeaderFollowers) | ✅ |
| `xi::current_trigger()` for script-side trigger access | ✅ |
| Recording + replay (observer mode + manifest + raw frames) | ✅ |
| Recording UI (start / stop / replay with speed picker) | ✅ |
| Multi-camera-synced reference plugin (`synced_stereo`) | ✅ |

### UI / dev experience

| Feature | Status |
|---|---|
| Interactive image viewer (pan + cursor-anchored zoom + pick tools) | ✅ |
| Inline image preview in plugin UI panels | ✅ |
| Project Settings webview | ✅ |
| Recent projects list | ✅ |
| Auto-respawn project replay after backend crash | ✅ |
| Crash report viewer with module blame | ✅ |
| Plugin tree with origin badges (project vs global) | ✅ |
| Variants / compare runs | ✅ |
| Remote backend mode (`--host 0.0.0.0 --auth`) | ✅ |

### Plugins shipped (`plugins/`)

`mock_camera`, `blob_analysis`, `data_output`, `json_source`,
`record_save`, `threshold_op`, `synced_stereo` — 7 plugins total.

The SDK (`sdk/examples/`) also ships demo plugins (`hello`, `counter`,
`invert`, `histogram`, `trigger_source`) as authoring references; these
are SDK examples, not production plugins.

---

## Process isolation — REMOVED 2026-05

The cross-process isolation mesh (worker / script-runner / shared-
memory region) was removed. The project pivoted to a single in-process
model: a frontend (FE) supervisor over a backend (BE) compute core
that calls ALL plugins — cameras included — directly in-process,
zero-copy via pointers. No SHM, no worker processes.

Rationale: a dead plugin means a dead pipeline regardless of
isolation, so per-plugin sandboxing only added complexity. Crash
diagnosability (minidumps + per-thread breadcrumbs + PDB
symbolication, see `guides/debugging.md`) is the replacement safety
net.

Removed with it: `xi_shm.hpp` and the host_api `shm_*` extensions
(the fields remain declared in the ABI struct for binary
compatibility but are always `nullptr`; plugins fall back to
`image_create`), `xinsp-worker.exe` / `xinsp-script-runner.exe`, the
`ProcessInstanceAdapter` / `ScriptProcessAdapter`, the `shm_metrics`
and `script_isolated_run` commands, and the `event:isolation_dead`.
The `instance.json` `"isolation"` field is now accepted but ignored
with a one-time deprecation warning, so old projects still load.
`docs/reference/ipc-shm.md` is retained for historical reference only.

---

## Test surface

See [`testing.md`](./testing.md) for the full breakdown. Summary:

- ~50 C++ unit tests across `xi_core`, `protocol`, `record`, `ops`,
  `image_pool`, `diagnostics`. (The Phase-3 SHM/IPC test set was
  deleted with the process-isolation removal.)
- ~30 Node integration tests under `vscode-extension/test/`.
- E2E suites driven by `@vscode/test-electron`: full pipeline,
  multi-camera, record/replay, user journey, project-plugin journey,
  UX states. Each suite captures screenshots of the running editor.

---

## Decision log (locked-in design choices)

- **No BPG protocol.** Everything over WS framing.
- **No N-API.** Backend is a standalone `xinsp-backend.exe`.
- **No graph authoring editor.** The script is the source of truth. A
  read-only pipeline graph view (`xinsp2.openPipelineGraph`) is
  available for visualisation, but graph-based authoring is not
  planned.
- **C++ compile path via MSVC `cl.exe`**; versioned DLL naming
  (`stem_vN.dll`) for Windows lock survival. No Cling / ClangREPL.
- **Stable C ABI for plugins.** No C++ types cross the plugin boundary.
- **Dependency-free host.** Only cJSON + stb_image_write vendored;
  OpenCV / IPP / turbojpeg optional via `XINSP2_HAS_*`.
- **VS Code is the IDE.** No in-house editor.
- **Headless backend.** Any WS client can drive it.
- **Single client at a time.** Multi-client deliberately deferred.
- **Per-instance folders.** Each instance owns `<project>/instances/<name>/`.
- **Trigger bus is opt-in.** Legacy `ImageSource` plugins continue to
  work unchanged.
- **Single in-process compute core.** Process isolation + SHM were
  removed 2026-05; FE supervisor over an in-process BE that calls all
  plugins directly. Crash diagnosability replaces sandboxing.

---

## Stretch goals (S-milestones, NewDeal sequence preserved)

| # | Name | Status |
|---|---|---|
| S1 | Live preview subscription | ✅ |
| S2 | Editor: auto-compile on save | ✅ |
| S3 | `xi::breakpoint("label")` | ✅ |
| S4 | Timeline / history (backend) | ✅ (UI scrubber TBD) |
| S5 | Operator library catalog | ✅ canny / open / close / adaptive / contours / matchTemplate |
| S6 | Multi-client broadcast | ❌ deliberately deferred |
| S7 | Recipe variant / A-B | ✅ `cmd:compare_variants` |
| S8 | Recording / replay | ✅ |
| S9 | Remote backend mode | ✅ |
| S10 | Headless production runner | ✅ |

---

## What's next

Priorities depend on real usage feedback. Candidate work:

- **FE/BE split boundary** (Task #94) — **skeleton shipped.** The frontend
  supervisor `xinsp-fe.exe` spawns/monitors the in-process backend compute
  core, drives the line to a safe state on backend death (via the
  `SafeStateSink` seam — logging stub now, real PLC transport later), and
  respawns it rate-limited. Cameras are BE plugins; the BE gains
  `--project/--script/--autostart-fps` to run headlessly. See
  [`design/fe-be-split.md`](./design/fe-be-split.md). Phase 2: deep WS
  heartbeat, FE status channel, real PLC transports.
- **PLC I/O** — the `xinsp-comms` gateway and `xi::comms` were removed; PLC
  transport is now a plugin concern (use `host->set_safe_state` for the
  emergency payload). See [`design/comms-gateway.md`](./design/comms-gateway.md).
- **Multi-client broadcast (S6)** — opens the door to operator dashboards.
- **History UI scrubber (finish S4)** — currently backend-only.
- **Per-component reference docs** — see [`docs/reference/`](./reference/).

For historical context (M0 vision, retired bug audits) see
[`docs/archive/`](./archive/).
