# xInsp2 — Development Plan

Execution order for the New Deal. Each milestone is independently testable
and leaves the tree in a runnable state. See `NewDeal.md` for the target
architecture.

## Milestone layout

| # | Milestone                                | Testable when                                           |
|---|------------------------------------------|---------------------------------------------------------|
| 0 | Core C++ headers (`xi::*`)               | Headers compile standalone; a demo `main.cpp` runs.     |
| 1 | WebSocket protocol definition            | `protocol/messages.md` + matching TS + C++ types lock.  |
| 2 | Backend service skeleton                 | `xinsp-backend.exe` listens on a port, echoes `cmd`.    |
| 3 | Value store + `vars` emission            | Running `inspect()` pushes variables to any WS client.  |
| 4 | JPEG preview path                        | A 20 MP image shows up in a browser tab via `<img>`.    |
| 5 | Compile-and-load user script             | `native_plugin` hot-reload works end-to-end over WS.    |
| 6 | `Instance<T>` + `Param<T>` registry      | Instance list appears; slider edits drive `exchangeCMD`.|
| 7 | VS Code extension skeleton               | Extension activates, connects, shows Variable Window.   |
| 8 | Instance UI reuse from xInsp             | Existing plugin UI components render inside a webview.  |
| 9 | First real inspection routine            | Folder → threshold → defect-count example runs.         |
|10 | Delete xInsp Electron graph code         | The old tree is gone, xInsp2 is the only shipped thing. |

---

## Milestone 0 — Core C++ headers

Self-contained, no deps beyond STL. Locks the user-facing API before any
transport work happens.

**Deliverables**
- `backend/include/xi/xi_async.hpp` — `Future<T>`, `xi::async()`, `ASYNC_WRAP`, `await_all`.
- `backend/include/xi/xi_var.hpp`   — `VAR(name, expr)` macro + thread-local value store.
- `backend/include/xi/xi_param.hpp` — `Param<T>` with range metadata.
- `backend/include/xi/xi_instance.hpp` — `Instance<T>` + `InstanceRegistry`.
- `backend/include/xi/xi.hpp`       — umbrella include.
- `examples/demo_async.cpp`         — minimal `inspect()` using only STL types.

**Exit test** — `examples/demo_async.cpp` compiles with any C++20 compiler
and produces expected output.

---

## Milestone 1 — WebSocket protocol definition

Write the message schema down before writing code. Six message types.

**Deliverables**
- `protocol/messages.md` — JSON schemas and binary frame layout.
- `backend/include/xi/xi_protocol.hpp` — C++ structs + JSON serialization helpers.
- `vscode-extension/src/protocol.ts` — TS types mirror of the same.

**Exit test** — A human can read `messages.md` and implement a compatible
client in any language.

---

## Milestone 2 — Backend service skeleton

Smallest possible server that loads nothing but the protocol.

**Deliverables**
- `backend/src/service_main.cpp` — uWebSockets server on a configurable port.
- `backend/CMakeLists.txt` — builds `xinsp-backend.exe`, pulls in uWebSockets.
- `cmd` handlers for: `ping`, `shutdown`, `version`.

**Exit test** — `websocat ws://localhost:7823` + manual JSON round-trip.

---

## Milestone 3 — Value store + `vars` emission

Wire `VAR` into the run path. At this point we have no user script yet; use
a hardcoded `inspect()` inside the backend.

**Deliverables**
- Thread-local value store (`xi_var.hpp`) serialized to `vars` on run
  completion.
- `run` command handler calls `inspect()`, waits, emits `vars`.
- Non-image types serialized to JSON inline (numbers, strings, booleans,
  tuples).
- Image variables serialized with a placeholder `gid` (no binary yet).

**Exit test** — WS client sends `run`, receives `vars` with the expected
entries.

---

## Milestone 4 — JPEG preview path

**Deliverables**
- `opencv` linked to backend.
- Per-image `preview` binary frame `[4B gid][4B codec][JPEG bytes]`.
- Quality setting via `settings.json` (default 85).
- `VAR_RAW` macro for uncompressed BMP streaming.

**Exit test** — A one-page HTML test client (`examples/test_client.html`)
connects, runs, and renders JPEG blobs in `<img>` tags via blob URLs.

---

## Milestone 5 — Compile-and-load user script

Bring `native_plugin`'s compile pipeline over. User writes `inspection.cpp`
in VS Code, saves, backend compiles it into a `.dll`, loads, and calls
`inspect()`.

**Deliverables**
- Port `native_plugin` compile driver into `backend/src/script_compiler.cpp`.
- `cmd: compile_and_load` — takes a path, returns success/error + build log.
- `cmd: unload_script` — releases the current `.dll`.
- Script ABI: `extern "C" void xi_inspect_entry(Image frame)`.

**Exit test** — Edit a local `inspection.cpp`, send `compile_and_load`,
then `run`, then see `vars` update. Edit again → reload → re-run.

---

## Milestone 6 — `Instance<T>` + `Param<T>` registry

Persist tunables and per-instance state to `project.json`.

**Deliverables**
- `InstanceRegistry` populates from `Instance<T>` ctors at script load time.
- `cmd: list_instances` / `list_params`.
- `cmd: set_param` / `cmd: set_instance_def` drive `exchangeCMD` and
  auto-save `project.json`.
- `cmd: load_project` / `save_project`.

**Exit test** — WS client changes a param value, backend persists it; restart
backend, project reloads, param retains the value.

---

## Milestone 7 — VS Code extension skeleton

Minimal extension that connects and shows a Variable Window.

**Deliverables**
- `vscode-extension/package.json` — contributions: activity bar icon,
  tree view, commands, webview panel, settings.
- `vscode-extension/src/extension.ts` — activate, WS client, spawn child
  backend process, register tree view and webview.
- `vscode-extension/src/wsClient.ts` — thin WS client (the only transport
  surface in the extension).
- `vscode-extension/webview/viewer/` — Vite-built React app: Variable
  Window + image preview panel. Uses the same IIFE pattern as existing
  plugin UIs.
- `vscode-extension/src/treeDataProvider.ts` — Instances tree.

**Exit test** — `F5` from the extension project launches an Extension
Development Host; xInsp2 sidebar appears; Run command executes inspection;
Variable Window populates.

---

## Milestone 8 — Instance UI reuse

Load existing xInsp plugin UI IIFE bundles inside webviews. No per-plugin
rewrite.

**Deliverables**
- `webview/instance/` — a per-instance webview panel that fetches the
  plugin UI bundle via `webview.asWebviewUri` and renders it.
- Shim `sendData` / `setDataChannel` to go over WS instead of N-API.
- Message routing by `instance_name` (replaces the `instance_id` routing
  that used to live in Electron renderer).

**Exit test** — Open `bright_thresh` from the tree view, its existing React
slider UI appears in a webview panel, slider moves drive live preview updates.

---

## Milestone 9 — First real inspection routine

End-to-end demo on a real image folder.

**Deliverables**
- `examples/folder_invert_save.cpp` — folder source → invert → save.
- `examples/defect_detection.cpp` — threshold → contours → classify.
- `examples/project.json` for each.
- Screenshots in `examples/screenshots/`.

**Exit test** — Clone the repo, open in VS Code, hit Run on an example,
see a defect-count update live.

---

## Milestone 10 — Delete xInsp Electron graph code

Only after M9 works end-to-end.

**Deliverables**
- Delete Electron main/renderer, preload, `addon.node`, `appExchangeClient`,
  graph editor components, BPG sources.
- Move any still-useful code (image viewer, plugin UI bundles) into xInsp2.
- Update top-level README to point at xInsp2.

---

## Crosscutting concerns

### Dependencies to add
- **uWebSockets** (C++ WS server) — header-only-ish.
- **nlohmann/json** (C++ JSON) — header-only.
- **libjpeg-turbo** — for fast JPEG encode. `cv::imencode` is fine to start.
- **OpenCV** — already used by existing plugins.

### Dependencies to drop
- Electron, N-API node-addon-api, Vite-for-Electron-renderer, React Flow.
- BPG protocol sources.

### Build system
- Keep CMake. Backend is an `.exe` target plus the existing plugin `.dll`s.
- VS Code extension builds with `npm run build` → `esbuild` or `vite` for
  the webview bundles.

### Testing
- **Unit**: each `xi_*` header has a test in `backend/tests/`.
- **Integration**: `examples/test_client.html` + a `node --test` Playwright
  client exercising real WS flows.
- **E2E**: VS Code extension test harness runs the extension against a
  local backend and drives commands.

### Thread safety invariants (from the start, not bolted on)
- Value store is per-run, per-thread. No cross-run sharing.
- `InstanceRegistry` is protected by a single mutex; script reload rebuilds
  it atomically.
- The WS server runs on its own thread; `run` dispatches to a worker
  thread. The server never blocks on user code.
- No condvar/wake plumbing. The server is request-driven; previews are
  sent synchronously from the worker thread's completion path.

---

## What we are NOT doing in v1

- No coroutines (`co_await`). Revisit if streaming cameras need it.
- No cancellation mid-run. A run always completes or crashes the worker.
- No Cling / JIT in-process. Stick with `.dll` reload from `native_plugin`.
- No AST rewriting for `VAR`. Macro form is fine; a Clang tool can come
  later if users ask.
- No remote multi-client support. Single client at a time.

---

## Order of operations for today

1. **M0: write the four headers** (`xi_async.hpp`, `xi_var.hpp`,
   `xi_param.hpp`, `xi_instance.hpp`) and the umbrella `xi.hpp`.
2. **M0 exit test**: write `examples/demo_async.cpp` and compile it with
   an available compiler to prove the API is sound.
3. **M1: write `protocol/messages.md`** and the matching type headers.
4. Pause and review. Transport work (M2) starts only after the API
   and protocol are locked.
