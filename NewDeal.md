# xInsp2 — New Deal

A rewrite of xInsp's frontend / plumbing layer that keeps the backend plugin
system intact and replaces everything above it. The goal is an HDevelop-style
authoring experience for machine-vision inspection routines, written as plain
C++ and edited in VS Code.

---

## Motivation

xInsp shipped with a React/Electron graph editor on top of an N-API shared-buffer
exchange into `libdlib.dll`. That architecture works but has three ongoing
problems:

1. **Node graphs are a poor DSL for inspection logic.** Branches, loops, helper
   functions, and shared state all fight the graph model. Real users want to
   write code, not drag boxes.
2. **The Electron+N-API pipe is fragile.** Shared-buffer framing (BPG), deque
   contiguity assumptions, wake-thread coordination, decoder state leftover,
   channel registration races — we spent whole debugging sessions on the
   transport, not the product.
3. **We're re-implementing a C++ editor.** Monaco-in-Electron can never match
   VS Code's IntelliSense, debugger, clang-format, git gutter, refactor, etc.
   That's the part users touch most and the part we have the weakest lever on.

HDevelop is the right reference point: **a sequential script with rich
variable inspection and a catalog of stateful operators**. We can get very
close to that with existing pieces by swapping the frontend and the transport.

---

## The model users see

Users write **one C++ file**:

```cpp
#include <xi/xi.hpp>

xi::Instance<Camera>      cam         { "cam0" };
xi::Instance<ShapeModel>  partTemplate{ "part_template" };
xi::Param<double>         sigma       { "sigma", 3.0, { 0.1, 10.0 } };
xi::Param<int>            lowT        { "canny_low", 50, { 0, 255 } };
xi::Param<int>            highT       { "canny_high", 150, { 0, 255 } };

void inspect(Image frame) {
    VAR(gray,    toGray(frame));
    VAR(blurred, gaussian(gray, sigma));

    auto p1 = async_canny(blurred, lowT, highT);
    auto p2 = async_findShape(gray, partTemplate);

    VAR(edges,   p1);                      // implicit await
    VAR(matches, p2);
    VAR(defects, filterDefects(edges, matches));

    report("result", defects);
}
```

Three primitives define the whole user API:

| Primitive        | Purpose                                              |
|------------------|------------------------------------------------------|
| `xi::Instance<T>`| Persistent, UI-backed state (templates, cameras).    |
| `xi::Param<T>`   | Tunable value with a slider / picker in the UI.      |
| `VAR(name, expr)`| Track and publish intermediate values for inspection.|

Parallelism comes from `xi::async(fn, args...)` + `Future<T>` with implicit
conversion. Each library operator also ships a pre-wrapped `async_<name>` form
via a `ASYNC_WRAP(name)` macro.

There is **no node graph**. The script *is* the graph.

---

## The architecture

Three decoupled processes talking over WebSocket:

```
┌───────────────────────────┐        ┌────────────────────────────┐
│ xinsp-backend.exe         │        │ VS Code extension          │
│  - libdlib / plugins      │  WS    │  - TreeView (instances)    │
│  - native_plugin (JIT .dll)│◀─────▶│  - Webview (viewer + vars) │
│  - WebSocket server        │  JSON  │  - File watcher on *.cpp   │
│  - JPEG encoder (turbojpeg)│ +binary│  - Commands palette        │
└───────────────────────────┘        └────────────────────────────┘
                ▲
                │  (any WS client — browser, CLI, Playwright, remote PC)
                ▼
```

### What survives from xInsp

Untouched:

- **`libdlib.dll`** — backend core.
- **Plugin system** — `PluginInstanceBase`, `exchangeCMD`, stage transport.
- **`native_plugin` compile pipeline** — user C++ → `.dll` → load.
- **Plugin UI React components** — render inside webviews via the same
  `BasePluginUI` IIFE pattern. Transport changes, component shape does not.
- **Core image types, OpenCV, stage helpers.**

### What gets deleted

- Electron main / renderer / preload.
- `addon.node` (N-API) and `exchangeDataInPlace`.
- BPG protocol (`BpgEncoder` / `BpgDecoder`, the deque framing, the wake
  callback, `stage_tx_queue_`).
- `appExchangeClient.ts`.
- Graph editor (nodes, edges, React Flow, `GraphEngine`).
- All IPC bridge code (`contextBridge`, preload).

### What gets added

- **`backend/src/service_main.cpp`** — standalone exe that loads libdlib and
  runs a WebSocket server. Replaces the Electron main process.
- **`backend/include/xi/xi_async.hpp`** — `Future<T>`, `xi::async(fn,args...)`,
  `ASYNC_WRAP(name)`.
- **`backend/include/xi/xi_instance.hpp`** — `Instance<T>` template and
  `InstanceRegistry`.
- **`backend/include/xi/xi_param.hpp`** — `Param<T>` with range metadata.
- **`backend/include/xi/xi_var.hpp`** — `VAR(name, expr)` macro, value store.
- **`backend/include/xi/xi.hpp`** — umbrella include.
- **`protocol/messages.md`** — WS message schema.
- **`vscode-extension/`** — extension host (activation, tree view, webview,
  file watcher, command palette contributions).

---

## Transport: WebSocket + JSON + JPEG

### Message kinds

Six types total. JSON text frames for control, binary frames for images.

| Type          | Direction | Payload                                                |
|---------------|-----------|--------------------------------------------------------|
| `cmd`         | UI → BE   | `{id, target, name, args}`                             |
| `rsp`         | BE → UI   | `{id, ok, data?, error?}`                              |
| `vars`        | BE → UI   | `{run_id, items:[{name,type,gid?,value?}]}`            |
| `preview`     | BE → UI   | binary: `[4B gid][4B codec][JPEG bytes]`               |
| `instances`   | BE → UI   | `{items:[{name,plugin,def}]}`                          |
| `log`         | BE → UI   | `{level,msg}`                                          |

No BPG, no shared buffer, no wake callback. WebSocket framing handles all
message boundaries; the backend sends one JPEG per binary frame.

### JPEG preview

- Encode with **libjpeg-turbo** (`cv::imencode` initially, native turbojpeg
  later). ~8–20 ms for 20 MP RGB at q=85.
- ~100× compression for typical photos → ~600 KB per 20 MP frame.
- Toggle per-variable: `VAR_RAW(name, expr)` streams uncompressed BMP for
  pixel-exact views (threshold tuning, etc.).

### Control/query flow

1. UI sends `{type:"cmd", name:"compile_and_load", args:{path:"..."}}`.
2. BE invokes `native_plugin` to compile `inspection.cpp` → `.dll`, loads it.
3. BE returns `rsp` with `ok` and declared instance/param metadata.
4. UI sends `{type:"cmd", name:"run"}`.
5. BE calls `inspect(frame)` on a worker thread.
6. `VAR(name, expr)` pushes into a per-run value store.
7. On completion, BE emits a `vars` message plus one `preview` binary frame
   per image variable.
8. UI updates Variable Window; webview renders JPEG blobs in `<img>`.

---

## Persistence

Two files, cleanly separated:

- **`inspection.cpp`** — the script: logic + instance and param *declarations*.
  Versioned in git. Edited by the user in VS Code.
- **`project.json`** — instance and param *configurations*: threshold values,
  template file paths, camera settings. Auto-saved on any UI mutation. Also
  versioned. Tuning never dirties the script.

This separation is the reason tuning feels instant — changing a slider
writes `project.json` and calls `exchangeCMD` on the instance; it never
triggers a C++ recompile.

---

## Parallelism story

`xi::async(fn, args...)` returns a `Future<T>` whose implicit conversion to
`T` acts as `.get()`. Thread pool under the hood (initially raw `std::async`,
later a Taskflow executor if profiling warrants).

Two forms:

```cpp
// Direct form
auto p1 = xi::async(featureA, gray);
auto p2 = xi::async(featureB, gray);
Image a = p1;                        // implicit await
Image b = p2;

// Pre-wrapped form (one line per operator in the op library header)
ASYNC_WRAP(gaussian)
ASYNC_WRAP(canny)

auto p1 = async_gaussian(gray, 3.0);
auto p2 = async_canny(gray, 50, 150);
```

Loops and conditionals work naturally — there is no lazy DAG to build.
Exceptions propagate through the implicit `.get()` site like JS await.

`xi::await_all(p1, p2, ...)` returns a tuple for the `Promise.all` case.

---

## Debugging story

Because the backend is a plain exe and the user script is a real `.dll`:

- **Breakpoints in user `inspection.cpp`** — attach VS Code cpptools to
  `xinsp-backend.exe`, step through `inspect()` with full watch support.
- **Variable Window** — every `VAR(...)` is observable after a run.
- **Live slider tuning** — `Param<T>` sliders drive `exchangeCMD` directly,
  no script recompile.
- **Browser debugging of the transport** — open devtools on any WS client,
  Network → WS → Messages shows every command and response.

This is categorically better than the Electron version, which had no script
debugger at all.

---

## Production deployment

The WebSocket split enables a real separation of authoring and production:

- **Authoring**: VS Code extension + local `xinsp-backend.exe`.
- **Production**: same `xinsp-backend.exe` on a factory PC, running the
  frozen `inspection.dll` headless. A thin UI (browser page or an operator
  HMI) connects over WS to display pass/fail and live preview. Same binary,
  different client.

---

## Non-goals (explicit)

- **Not rewriting the plugin system.** Existing `PluginInstanceBase` plugins
  compile into xInsp2 unchanged.
- **Not rewriting OpenCV / Halide / stage types.** The operator library
  keeps its current shape — we add the `ASYNC_WRAP` layer on top.
- **Not building a custom IDE.** VS Code is the IDE. We ship an extension.
- **Not maintaining BPG.** WebSocket framing subsumes it.
- **Not shipping a graph editor.** Delete on sight.

---

## Success criteria

1. A user can open xInsp2 in VS Code, create `inspection.cpp`, write a
   sequential routine using `xi::*` primitives, hit Run, and see every
   tracked variable update in a side panel.
2. Tuning a `Param<T>` slider in the UI updates the live preview within
   one frame, with no recompile.
3. Independent operator calls run in parallel when wrapped with `async_*`.
4. Breakpoints set in `inspection.cpp` from VS Code hit during a run.
5. The backend runs headless on a remote machine and a browser-only client
   can connect, run the script, and see results.
6. Total new code under 3 kLOC excluding vendored libs.
