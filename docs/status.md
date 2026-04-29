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

Master holds **9 shipped plugins** and ships the cross-process
isolation mesh as an **opt-in** (merged from the
`shm-process-isolation` spike). Set `"isolation": "process"` per
instance, or call `cmd:script_isolated_run` for scripts. Default-on is
tracked work — see *Process isolation* below.

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

## Process isolation (merged, opt-in)

Merged from the `shm-process-isolation` spike. All 9 SHM/IPC tests
green. Default is in-proc; per-instance opt-in via
`"isolation": "process"`.

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
- **Always-on reader thread** (Task #74 / PR #19 follow-up) — each
  `ProcessInstanceAdapter` runs a dedicated thread that owns the pipe's
  read side. Replies for in-flight RPCs fulfil per-seq promises;
  `seq=0` async frames (`RPC_EMIT_TRIGGER`) dispatch straight to
  `TriggerBus` whether or not a backend→worker call is in flight.
  This unblocks `"isolation":"process"` for source plugins; previously
  triggers piled up unread until the next backend RPC. Exercised by
  `examples/cross_proc_trigger/`.
- **Worker-side conveniences merged with the spike**:
    - heap-pool → SHM auto-copy in `worker_main` so plugins that use
      `xi::Image{...}` (the common case) work cross-process without
      knowing they're isolated.
    - Output image key preserved across the IPC boundary (e.g.
      `record.image("mask", img)` still surfaces as `"mask"` to the
      script, not as a fixed `"out"`).
- **Default**: in-proc for both plugin instances and scripts.
  Per-instance `"isolation": "process"` opts plugins in;
  `cmd:script_isolated_run` opts scripts in.
- **Default-on tracked**: needs broader real-plugin testing
  (multi-image outputs, plugins that store image handles in their
  JSON, error / hot-reload paths) plus folding script isolation into
  `cmd:run` while preserving previews / history / watchdog.

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
- **Process isolation: opt-in.** Set `"isolation":"process"` per
  instance or call `cmd:script_isolated_run` for scripts. Default-on
  is gated on broader plugin coverage + script-side preview wiring.

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

- **Process-isolation default-on.**
    - Instance side: needs real-plugin coverage beyond the 9 spike
      tests — multi-image Records, plugin-side handle storage in JSON,
      hot-reload semantics, fallback / fail-loud policy.
    - Script side: refactor `cmd:run` to host the script in
      `xinsp-script-runner.exe` while preserving binary previews,
      history, watchdog, breakpoint, continuous mode. The runner +
      `ScriptProcessAdapter` exist — the work is wiring snapshot vars
      + SHM-resolved gids back through `emit_vars_and_previews`.
- **Multi-client broadcast (S6)** — opens the door to operator dashboards.
- **History UI scrubber (finish S4)** — currently backend-only.
- **Per-component reference docs** — see [`docs/reference/`](./reference/).

For historical context (M0 vision, retired bug audits) see
[`docs/archive/`](./archive/).
