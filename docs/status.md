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

Master holds **9 shipped plugins** and now ships **process-isolated
plugin instances by default** (merged from the `shm-process-isolation`
spike — see *Process isolation* below). User-script isolation remains
opt-in via `cmd:script_isolated_run`.

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
`record_save`, `synced_stereo`, plus three demo levels in the SDK
(`hello`, `counter`, `invert`, `histogram`, `trigger_source`).

---

## Process isolation (merged, instance side default-on)

Merged from the `shm-process-isolation` spike. All 9 SHM/IPC tests
green; instance default flipped to process so plugin AVs / heap
corruption no longer take the backend with them.

- `xi_shm.hpp` — Windows `CreateFileMapping`-backed buffer pool with
  cross-process atomic refcount and bump allocator (512 MB region per
  backend).
- `host_api` extensions: `shm_create_image` / `shm_alloc_buffer` /
  `shm_addref` / `shm_release` / `shm_is_shm_handle` (binary-compatible
  append — pre-isolation plugin DLLs still load).
- `xinsp-worker.exe` — hosts ONE plugin instance in its own process.
  Method calls go over a named pipe; pixel data rides SHM (zero-copy).
- `xinsp-script-runner.exe` — analogous host for user scripts.
- `ProcessInstanceAdapter` + `ScriptProcessAdapter` — host-side handles
  with auto-respawn (rate-limited 3/60s) and per-call timeout via
  `CancelIoEx` watchdog.
- **Default for plugin instances**: process. Opt out per-instance with
  `instance.json: "isolation": "in_process"`. Falls back to in-proc
  with a warning if `xinsp-worker.exe` isn't on disk.
- **User-script isolation**: still opt-in via the
  `cmd:script_isolated_run` command. Default `cmd:run` continues to
  load the script DLL in-proc (so it keeps emitting binary previews,
  history entries, etc.). Folding script-isolation into `cmd:run` is
  next-up work — see "In flight" below.

---

## Test surface

See [`testing.md`](./testing.md) for the full breakdown. Summary:

- ~50 C++ unit tests across `xi_core`, `protocol`, `record`, `ops`,
  `image_pool`, `diagnostics`, plus Phase-3 SHM/IPC tests on the spike
  branch (~9 more).
- ~30 Node integration tests under `vscode-extension/test/`.
- E2E suites driven by `@vscode/test-electron`: full pipeline,
  multi-camera, record/replay, user journey, project-plugin journey,
  UX states. Each suite captures screenshots of the running editor.

---

## Decision log (locked-in design choices)

- **No BPG protocol.** Everything over WS framing.
- **No N-API.** Backend is a standalone `xinsp-backend.exe`.
- **No graph editor.** Script-first authoring UX.
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
- **Process isolation: instance side default-on.** Every plugin instance
  spawns a worker; opt out per-instance with `"isolation":"in_process"`.
  User scripts are still in-proc by default (binary-preview path needs
  SHM rewiring before the default flip is safe).

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

- **Script-side isolation by default.** Refactor `cmd:run` to host the
  script in `xinsp-script-runner.exe` while preserving binary previews,
  history, watchdog, breakpoint, and continuous mode. The runner +
  `ScriptProcessAdapter` already exist — the work is wiring snapshot
  vars + SHM-resolved gids back through `emit_vars_and_previews`.
- **Multi-client broadcast (S6)** — opens the door to operator dashboards.
- **History UI scrubber (finish S4)** — currently backend-only.
- **Per-component reference docs** — see [`docs/reference/`](./reference/).

For historical context (M0 vision, retired bug audits) see
[`docs/archive/`](./archive/).
