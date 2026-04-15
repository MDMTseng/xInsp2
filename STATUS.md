# xInsp2 — Status & Forward Plan

Updated as the build progresses. `NewDeal.md` is the stable vision;
`DEV_PLAN.md` is the milestone skeleton; this file is the weekly pulse.

## Where we are

**Complete:**
- **M0 — core C++ headers** (`xi_async.hpp`, `xi_var.hpp`, `xi_param.hpp`, `xi_instance.hpp`, `xi.hpp`). 9 assertions.
- **M1 — protocol schema** (`messages.md`, `xi_protocol.hpp`, `protocol.ts`, fixtures). C++ and TS parse the same fixtures. 8 protocol + 3 TS assertions.
- **M2 — backend service skeleton** (`xinsp-backend.exe`, `xi_ws_server.hpp` header-only WS lib, no external deps). `ping`/`version`/`shutdown` over real WebSocket. 5 integration assertions.
- **M3 — run + vars emission** (`cmd: run` executes a hardcoded `inspect()`, walks `ValueStore`, emits `vars`; `set_param` mutates tunables and re-runs pick up new values; `list_params` returns the registry). 3 integration assertions.

**Tests:** 9 unit + 8 protocol + 3 TS + 5 WS basic + 3 WS run = **28 regression assertions**, all green. Total runtime under 5 seconds.

**Code budget:** ~1.8 kLOC new (headers, service, tests, docs). Target from NewDeal is 3 kLOC.

## Goal vs. current state

| Success criterion                                              | State                          |
|----------------------------------------------------------------|--------------------------------|
| Sequential `xi::*` script with live variable inspection        | Backend ready, no UI yet       |
| Slider tuning → live preview, no recompile                     | `set_param` works, no preview  |
| Parallel ops via `async_*`                                     | Proven                         |
| Breakpoints in `inspection.cpp` hit                            | Needs M5 + cpptools integration|
| Remote browser client can run the script                       | Transport ready, script hardcoded|
| <3 kLOC new code                                               | 1.8 kLOC, on track             |

## Immediate next milestones

### M4 — Image variable type + JPEG preview (≈ 1–2 days)

- Add an `Image` struct in `xi_image.hpp` (width, height, channels, data pointer, ownership policy). Initially a thin wrapper around a `std::vector<uint8_t>` pixel buffer — no OpenCV dependency yet.
- Specialize `VarTraits<Image>` so `VAR(x, someImage)` registers as `VarKind::Image`.
- Encode to JPEG via `libjpeg-turbo` (or `cv::imencode` if we pull in OpenCV for real). Start with libjpeg-turbo: ~2 MB dep, much faster build.
- After `vars`, send one `preview` binary frame per image variable with the 20-byte header + JPEG bytes.
- Add `VAR_RAW(x, img)` path that sends BMP instead of JPEG.
- Integration test: run an inspection that produces a synthetic checkerboard `Image`, decode the JPEG in Node via `sharp` or `Jimp`, assert dimensions match.

### M5 — Compile-and-load user script (≈ 2–3 days)

- Port the C++ compile driver from `xInsp/plugins/native_plugin` (it wraps MSVC `cl.exe` / g++). Live in `backend/src/script_compiler.cpp`.
- ABI: user script declares `extern "C" void xi_inspect_entry(const xi::Image& frame)` and is compiled as a shared library against `xi/xi.hpp`.
- Cycle: `cmd: compile_and_load` → (optionally `unload_script` first) → invoke compiler → `LoadLibrary` → resolve symbol → cache handle.
- Script-side constructors for `xi::Instance<T>` and `xi::Param<T>` run on load and populate registries; `unload_script` clears both.
- Error path: compile failures return `ok:false` with a `build_log` in `data`.
- Integration test: write a minimal `inspection.cpp` to a temp file, `compile_and_load`, `run`, assert the expected `vars`. Modify, recompile, assert new result.

### M6 — Instance adapter + project.json persistence (≈ 1 day)

- `Instance<T>` factory specialization that creates a real plugin instance the adapter routes to `PluginInstanceBase::exchangeCMD` from the old xInsp tree. (For xInsp2 pure plugins, the adapter is a no-op — the user's class already derives from `xi::InstanceBase`.)
- `save_project` writes `project.json` with every param value and every instance's `get_def()`.
- `load_project` reads the same and calls `set_param` / `set_def` on the registries after a `compile_and_load`.
- Auto-save on any `set_param` or `set_instance_def` that succeeds.
- Integration test: create params/instances, set values, `save_project`, restart backend, `load_project`, assert values match.

### M7 — VS Code extension skeleton (≈ 2 days)

- `vscode-extension/package.json` contributions: `xinsp.run`, `xinsp.compile`, activity bar icon, tree view for instances/params, webview view for the viewer panel.
- `extension.ts`: spawn `xinsp-backend.exe` on activation, connect, route messages to the webview.
- `wsClient.ts`: the thin WS client we already have in tests, promoted to a real module.
- `viewer/` webview (Vite + React): Variable Window listing all tracked vars with type-specific renderers (number, string, boolean, image).
- `treeDataProvider.ts`: tree view backed by `InstancesMsg`.
- Manual gate: open the extension in an Extension Development Host, hit Run, see the Variable Window update. The moment the loop closes.

### M8 — Instance UI reuse (≈ 1 day if xInsp plugin UIs port cleanly)

- Host existing xInsp plugin UI IIFE bundles inside webview panels.
- Shim `sendData` / `setDataChannel` over the new WS `cmd: exchange_instance` passthrough.

### M9 — First real inspection routine (≈ 1 day)

- `examples/folder_invert_save.cpp` — folder source plugin → invert → folder saver, all via `xi::Instance<T>` + `VAR(...)`. Shipped in the repo. Used as the canonical smoke test.

### M10 — Delete old xInsp electron/graph code (≈ 0.5 day)

- Delete when M9 is green. Move any still-useful assets into xInsp2.

## New work items surfaced during implementation

These showed up during M0–M3 and are worth capturing before they get lost:

- **Encoding warnings on Windows (C4819)**: MSVC reads our UTF-8 files as CP950. Fixed via `/utf-8` in the backend CMake. Watch for a re-appearance if any file is saved without a BOM.
- **`getenv` deprecation warning** in `test_protocol.cpp`: harmless, suppress with `_CRT_SECURE_NO_WARNINGS` in the test target only, or swap to `_dupenv_s`. Low priority.
- **Random test ports** can collide. Current PRNG is `Math.random()`. If flake appears in CI, switch to `net.createServer({ port: 0 })` to let the OS pick.
- **`xi_ws_server.hpp`** is Windows-first. Linux path is there but untested — gate on M10 so we don't distract from progress.
- **Exception propagation in async + VAR**: if a `Future<T>` used inside `VAR()` throws at the implicit conversion, the ValueStore entry for that name is not inserted (the throw escapes `track()`'s `copy_for_store = value`). Semantically correct but worth a test + doc line.
- **No cancellation mid-run**: deliberate non-goal. Document so nobody tries to add `std::stop_token` later.

## Milestones beyond M10 (stretch)

These are the "ideal app" features — aspirational, not committed.

### S1 — Live preview subscription model
- `subscribe`/`unsubscribe` commands already in the protocol schema. Implement so big-image variables only stream when the viewer is actually looking at them.

### S2 — Real C++ editor integration
- Detect a `inspection.cpp` file change in VS Code, auto-trigger compile_and_load, show build errors as diagnostics in the Problems panel (via `vscode.DiagnosticCollection`). One file-watcher, ~50 lines.

### S3 — Breakpoint-style step debugging
- Expose a `xi::breakpoint("label")` helper that emits an `event` and blocks on a condvar until the client posts `cmd: resume`. Cheap poor-man's debugger. Real cpptools step-through comes for free once users attach to `xinsp-backend.exe`.

### S4 — Timeline / history
- Keep the last N runs' `ValueStore` snapshots. A viewer scrubber replays any historical frame's variables without re-running inspection. Invaluable for tuning loops.

### S5 — Operator library catalog
- Standard set of wrapped operators: `toGray`, `threshold`, `gaussian`, `canny`, `findContours`, `matchTemplate`, etc. Each one ships via `ASYNC_WRAP(name)`. Users `#include <xi/ops.hpp>` and go. This is the biggest usability win and is mostly glue code on top of OpenCV.

### S6 — Multi-client broadcast
- Single-client is a stated non-goal for v1, but the protocol layer is almost ready for multi-client broadcast (preview frames go to all subscribers). Add a session id to the protocol and a `std::vector<Client>` in the server. Small change, big deployment flexibility.

### S7 — Recipe variant / A-B compare
- Save two variants of `project.json` and the viewer lets the user flip between them with the same frame. Enables "what does sigma=3 vs sigma=4 look like" in one click.

### S8 — Recording and replay
- Dump a run's input frame + every tracked var + timing to disk. Load it back later, step through, share with teammates. Essentially unit-test-from-production.

### S9 — Remote backend mode
- Bind WS to `INADDR_ANY` behind a command-line flag, add a shared-secret auth header (simple `Authorization: bearer ...`), and the VS Code extension can connect to a factory PC. Unlocks the "authoring on laptop, inspection on machine" story from NewDeal.md. Maybe 50 lines.

### S10 — Headless production runner
- A second small binary `xinsp-runner.exe` that loads the compiled `inspection.dll`, grabs frames from a camera, and outputs pass/fail + a JSON report. No WS, no UI, no frontend. This is the production deployment mode.

## Decision log (things locked in already)

- **No BPG protocol.** Everything over WS framing.
- **No N-API.** Backend is a standalone exe.
- **No graph editor.** Script-first UX.
- **C++ compile path via native_plugin's driver**, not Cling / ClangREPL. Dll reload is fast enough.
- **Dependency-free core.** No uWebSockets, no nlohmann/json. The built-in `xi_ws_server.hpp` and `xi_protocol.hpp` cover the protocol surface we need. Replace only when genuine feature pressure shows up.
- **VS Code is the IDE.** No in-house editor.
- **Headless backend.** The UI is optional — any WS client can drive it.
