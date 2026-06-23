# xInsp2

**An HDevelop-style machine-vision inspection framework — written as plain
C++, edited in VS Code, wired over WebSockets.**

> You write one C++ file. xInsp2 gives you a live Variable Window, tunable
> sliders, hot-reload, crash isolation, a plugin SDK, multi-camera
> trigger correlation, record/replay, remote backend, and a headless
> production runner.

> 🚀 **New to the project?** Read **[`docs/overview.md`](docs/overview.md)**
> first — the mental model, the architecture in one picture, build-and-run on day
> one, and a guided index into all the docs.

---

## The model

```cpp
#include <xi/xi.hpp>           // xi::Image, xi::Param, VAR, OpenCV (cv::*)
#include <xi/xi_use.hpp>

xi::Param<int>    thresh {"threshold", 128, {0, 255}};
xi::Param<double> sigma  {"sigma",     2.0, {0.1, 10.0}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& det = xi::use("detector0");         // survives hot-reload

    auto t = xi::current_trigger();           // frames pushed by the cam0 source
    if (!t.is_active()) return;
    auto img = t.image("frame");
    if (img.empty()) return;

    VAR(input, img);                          // tracked & visible in UI

    cv::Mat gm, bm;
    cv::cvtColor(img.as_cv_mat(), gm, cv::COLOR_RGB2GRAY);
    int k = (int)(sigma * 2 + 1) | 1;
    cv::GaussianBlur(gm, bm, cv::Size(k, k), (double)sigma);
    xi::Image blur(bm.cols, bm.rows, 1, bm.data);

    auto result = det.process(xi::Record()
        .image("gray", blur)
        .set("threshold", (int)thresh));      // slider value, no recompile

    VAR(detection, result);
    VAR(pass, result["blob_count"].as_int() <= 3);
}
```

Three primitives do all the heavy lifting:

| Primitive        | Role                                                       |
|------------------|------------------------------------------------------------|
| `xi::Instance<T>`| Persistent, UI-backed state (cameras, templates, models)   |
| `xi::Param<T>`   | Tunable value with a slider / picker in VS Code            |
| `VAR(name, expr)`| Track and publish an intermediate value for inspection     |

Parallelism is `xi::async(fn, args...)` + `Future<T>` with implicit
await. Trigger-correlated multi-camera capture is
`xi::current_trigger()`. **Script-first authoring.** A read-only
pipeline graph view (`xinsp2.openPipelineGraph`) is available, but the
script is the source of truth — there is no graph-based authoring
editor.

---

## Architecture

```
┌──────────────────────────────┐       ┌────────────────────────────────┐
│ xinsp-backend.exe            │       │ VS Code extension              │
│   ├─ plugin scan + cert      │       │   ├─ Instances / Params tree   │
│   ├─ native_plugin JIT dll   │  WS   │   ├─ CodeLens on .cpp          │
│   ├─ ImagePool (sharded)     │◀─────▶│   ├─ Viewer (vars + preview)   │
│   ├─ TriggerBus              │ JSON  │   ├─ Plugin UI webviews        │
│   ├─ WebSocket server        │+JPEG  │   └─ runMulticam / runRecord…  │
│   └─ SEH crash translator    │       │                                │
└──────────────────────────────┘       └────────────────────────────────┘
        ▲                 ▲
        │                 │ (any WS client — browser, CLI, remote PC)
        │                 │
        │        ┌────────┴────────────────┐
        │        │ xinsp-runner.exe        │  Headless. No WS. Takes a
        │        │   loads project.json,   │  project folder and writes
        │        │   compiles, runs N      │  a pass/fail JSON report.
        │        │   frames, writes report │
        │        └─────────────────────────┘
        │
   plugin DLLs (C ABI, trusted load)
   user inspect.dll (JIT-compiled)
```

Key design choices:

- **Stable C ABI for plugins.** No C++ types cross `xi_plugin_*` boundary —
  survives MSVC version drift.
- **Sharded refcounted ImagePool.** 16 shards, `shared_mutex`, 64-bit
  internal counter.
- **SEH → C++ exception translation.** Null deref, div/0, array overrun,
  C++ throw — all recoverable without killing the backend.
- **Hot-reload with state.** `xi::state()` serialises before DLL unload,
  restores after. Script edits are a one-file recompile.
- **Dispatch.** Sources `emit_record` (images + metadata under a 128-bit
  trigger id); the host dispatches one inspection per emit. Multi-camera
  correlation is a gathering plugin (e.g. `synced_stereo`), not a policy.
- **Lean host.** Only yyjson + stb_image vendored. **OpenCV is required**
  (image ops + every script/plugin force-includes it); libjpeg-turbo and
  IPP are optional accelerators behind `XINSP2_HAS_*`.

---

## Install — end users

The fastest way: grab the prebuilt zip from the
[Releases page](https://github.com/MDMTseng/xInsp2/releases/latest).

1. **Download** `xinsp2-<version>-win-x64.zip` and unzip somewhere stable
   (e.g. `C:\xinsp2`).
2. **Install the VS Code extension**: in VS Code, `Extensions` (Ctrl+Shift+X)
   → `…` menu → `Install from VSIX…` → pick
   `extension/xinsp2-<version>.vsix` from the unzipped folder.
3. **Verify**:

   ```bat
   C:\xinsp2\bin\xinsp-backend.exe --version
   ```

   should print `xinsp-backend <version> (<commit>)`.

The extension auto-spawns the backend on activation. If you put the
folder somewhere other than the dev tree, set the absolute path in
VS Code settings:

- `xinsp2.backendExe` = `C:\xinsp2\bin\xinsp-backend.exe`

No CMake or Node is needed just to **run** xInsp2. Note, however, that
the backend compiles the inspection script (and project-local plugins)
on the fly with `cl.exe`, so the **MSVC C++ toolchain and OpenCV must be
present at runtime** unless you ship pre-compiled script/plugin DLLs for a
locked production line. See [`docs/guides/build-and-run.md`](docs/guides/build-and-run.md)
for the full per-machine setup, and the in-editor **Project Settings → C++
Toolchain** panel to verify a machine has everything (it warns on missing or
wrong paths and lets you pin per-project overrides).

---

## Usage walkthrough

This is the 10-step path our automated `runUserJourney` test takes —
build a project from zero, configure 3 instances, write a script, run
inspections, save outputs.

### 1. Open the xInsp2 sidebar

Click the beaker icon on the activity bar. The Welcome view shows up
with starter actions.

![Welcome view](docs/screenshots/initial_welcome.png)

### 2. Add an instance

After clicking **Create New Project**, the **Instances & Params** view
opens. Hit `+` to add an instance — the QuickPick lists every
discovered plugin (cameras, detectors, savers, …).

![Plugin picker](docs/screenshots/add_cam0_a_plugin_picker.png)

Add three: `cam0` (mock_camera), `det0` (blob_analysis), `saver0`
(record_save). They show up in the tree with inline icons.

![Three instances + plugin tree](docs/screenshots/instances_created.png)

### 3. Configure cam0 — live preview

Click `cam0`'s gear icon. The mock_camera plugin's webview opens. Set
FPS, click **Start**, and watch the live JPEG stream in the preview pane.

![Camera streaming](docs/screenshots/camera_streaming.png)

### 4. Configure det0 — blob analysis

Slide threshold, set min/max area, click **Apply & Run**. The plugin's
canvas overlay highlights detected blobs.

![Blob analysis applied](docs/screenshots/blob_applied.png)

### 5. Configure saver0 — wire up disk output

Set the output directory + naming rule, hit **Enable**.

![Saver enabled](docs/screenshots/saver_enabled.png)

### 6. Write the inspection script

`inspect.cpp` was created when you made the project. Edit it — same
buffer style as any C++ file (IntelliSense, save, format).

```cpp
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& det   = xi::use("det0");
    auto& saver = xi::use("saver0");

    auto t = xi::current_trigger();        // frames pushed by the cam0 source
    if (!t.is_active()) return;
    auto img = t.image("frame");
    if (img.empty()) return;

    VAR(input, img);
    VAR(detection, det.process(xi::Record().image("gray", img)));
    saver.process(xi::Record().image("input", img));
}
```

### 7. Compile

Click the gear icon in the editor title bar (visible when on
`inspect.cpp`). Build runs in seconds, hot-reloads the DLL.

![After compile](docs/screenshots/after_compile.png)

### 8. Run

Click the `▷` icon in the Instances view title bar (or hit **Ctrl+F5**).
Each `VAR()` lights up in the Variable Window with type-specific
renderers — numbers, booleans, image thumbnails, Record trees.

![Inspections ran — viewer populated](docs/screenshots/inspections_ran_viewer.png)

### 9. Headless production run

For factory deployment without VS Code, use `xinsp-runner.exe`:

```bat
bin\xinsp-runner.exe path\to\project --frames=1000 --output=today.json
```

The runner loads `project.json`, restores all instances, compiles
the script, runs N frames, and writes a JSON report — no WS, no UI,
no UI dependencies. Exit `0` if every frame ran clean.

### 10. Remote backend (LAN deployment)

On the factory PC:

```bat
xinsp-backend.exe --host=0.0.0.0 --port=7823 --auth=<shared-secret>
```

In VS Code on the developer laptop:

- `xinsp2.remoteUrl` = `ws://factory-pc.lan:7823`
- `xinsp2.authSecret` = `<shared-secret>`

The extension connects over the network instead of spawning locally.
Same UI, real-machine images.

---

## Build from source — developers

If you want to modify the framework itself (not just write plugins):

### Prerequisites

- Windows 10/11 (Linux build path WIP)
- **MSVC 2019+** with the *Desktop development with C++* workload (or clang-cl) — `cl.exe` + `vcvars64.bat` + Windows SDK
- **OpenCV 4.x** at `C:\opencv\opencv\build` (or set `OpenCV_DIR`) — **required** (`find_package(OpenCV REQUIRED)`; every script/plugin force-includes `<opencv2/opencv.hpp>`)
- CMake ≥ 3.16
- Node.js 18+ (extension build + tests)
- **Optional accelerators** (auto-detected; install any you want):
  - **libjpeg-turbo** at `C:\libjpeg-turbo64` — fast JPEG encode (`winget install libjpeg-turbo.libjpeg-turbo.VC`)
  - **Intel IPP 2026+** at `C:\Intel\ipp\<ver>` — image-op SIMD acceleration

See [`docs/guides/build-and-run.md`](docs/guides/build-and-run.md) for step-by-step new-machine setup.

### Build

```bash
# Backend + runner
cmake -S backend -B backend/build -A x64 \
    -DXINSP2_HAS_OPENCV=ON \
    -DXINSP2_HAS_TURBOJPEG=ON \
    -DXINSP2_HAS_IPP=ON
cmake --build backend/build --config Release

# Plugins (mock_camera, blob_analysis, synced_stereo, …)
cmake -S plugins -B plugins/build -A x64
cmake --build plugins/build --config Release

# VS Code extension
cd vscode-extension && npm install && npm run build
```

Runtime DLLs (OpenCV / turbojpeg / IPP) are auto-copied next to
`xinsp-backend.exe` by the build — no PATH munging needed.

### Run in VS Code (dev mode)

Open the repo in VS Code and hit `F5`. An Extension Development Host
launches with xInsp2 wired up; the auto-detection finds the backend
under `backend/build/Release/`.

### Build a release zip

```bash
node tools/build_release.mjs
# → release/xinsp2-<version>-win-x64.zip
```

This is the same script that produced the file on the Releases page.

---

## What's in the box

| Component             | Purpose                                                     |
|-----------------------|-------------------------------------------------------------|
| `backend/`            | The core: `xi::*` headers, WS server, ImagePool, TriggerBus |
| `backend/src/service_main.cpp` | `xinsp-backend.exe` — full interactive server      |
| `backend/src/runner_main.cpp`  | `xinsp-runner.exe` — headless production runner    |
| `vscode-extension/`   | VS Code integration: TreeView, CodeLens, webviews, E2E      |
| `plugins/`            | Shipped plugins: `mock_camera`, `blob_analysis`, `data_output`, `json_source`, `record_save`, `threshold_op`, `synced_stereo` |
| `sdk/`                | Plugin SDK: `scaffold.mjs`, `cmake/` module, `template/`, `testing/` helpers, worked examples |
| `examples/`           | User-script examples (defect_detection, use_demo, …)        |
| `docs/`               | Architecture, status, testing, protocol reference, guides   |

---

## Features

### Inspection authoring

- **One-file scripts.** Include `<xi/xi.hpp>`; write a plain C++ function.
- **Variable Window.** Every `VAR(name, expr)` shows up live with a
  type-specific renderer (number, bool, string, Image preview, Record tree).
- **Live tuning.** `xi::Param<T>` sliders drive `set_param` directly; no
  recompile. `set_param` → next `run` picks up the new value.
- **Parallel ops.** `xi::async(fn, args...)` + `Future<T>` with implicit
  await. `ASYNC_WRAP(name)` to pre-wrap an operator.
- **Record type.** `rec["roi.x"].as_int(0)`, `rec["items[0].score"].as_double()`
  — path expressions, safe defaults, named-image bag, yyjson-backed.

### Operational

- **Hot-reload.** Save `.cpp` → backend recompiles → instance state
  survives (`xi::state()`, `get_def`/`set_def`).
- **Crash isolation.** SEH `_set_se_translator` wraps every script /
  plugin call site. A null deref in user code returns an error message;
  the backend stays up.
- **CodeLens.** `⚙ Configure` / `🎚 Tune` / `👁 Preview` on every
  `xi::use(...)` / `xi::Param<...>` / `VAR(...)` site.

### Image sources & dispatch

- **`xi::emit_record(host, source, record)`** — source plugins push a
  record (images + metadata) into the pipeline; the host stamps a 128-bit
  trigger id and dispatches the inspection once per emit.
- **Script API:** `xi::current_trigger().image("frame")`, `.id_string()`,
  `.meta()`, `.sources()`.
- **Multi-source correlation** (e.g. a synced stereo pair) is plugin
  composition, not a bus policy: a **gathering plugin** subscribes to the
  sources, combines their frames into one record, and emits that. See
  `examples/stereo_sync/`.

### Recording & replay

- **Replay is a plugin.** The **buffer_replay** plugin captures emitted
  records and re-emits them through the same `emit_record` path, so the
  whole pipeline sees them identically to a live run — for regression
  tests and off-line tuning. See `examples/buffer_replay_demo/`.

### Deployment

- **Remote backend.** `--host=0.0.0.0 --auth=<secret>` opens the bus to
  the network; clients send `Authorization: Bearer <secret>` in the WS
  handshake. Constant-time compare.
- **Headless runner.** `xinsp-runner.exe <project>` — no WS, no UI.
  Loads `project.json`, compiles the script, runs N frames, writes a
  JSON report. The production face of xInsp2.

### Plugins

Write a plugin in ~30 lines of C++:

```cpp
#include <xi/xi_abi.hpp>

class MyPlugin : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    xi::Record process(const xi::Record& input) override {
        int t   = input["threshold"].as_int(128);
        auto in = input.get_image("gray");
        auto out = pool_image(in.width, in.height, 1);
        cv::threshold(in.as_cv_mat(), out.as_cv_mat(), t, 255, cv::THRESH_BINARY);
        return xi::Record().image("dst", out).set("t_used", t);
    }
};
XI_PLUGIN_IMPL(MyPlugin)
```

Scaffolding:

```bash
node <xinsp2>/sdk/scaffold.mjs ~/my_plugins/my_first_plugin
```

Plugins are **trusted** — the host loads the DLL straight through after an
ABI-version check (no certification gate). Write your own tests with
`xi_test.hpp` to gain confidence; nothing blocks a plugin from loading.

Full SDK docs: [`sdk/README.md`](sdk/README.md) and
[`sdk/GETTING_STARTED.md`](sdk/GETTING_STARTED.md).

---

## Repo map

```
xInsp2/
├── README.md                ← you are here
├── docs/
│   ├── architecture.md      ← technical map: components, data flow, lifecycles
│   ├── status.md            ← what's shipping / WIP / planned (no parallel files)
│   ├── testing.md           ← test surface + how to run + how to add
│   ├── protocol.md          ← WebSocket wire-format reference
│   ├── guides/              ← task-shaped onboarding (adding a plugin, debugging, …)
│   ├── reference/           ← per-API references (host_api, plugin-abi, instance-model, …)
│   └── archive/             ← historical snapshots (M0 plan, retired audits)
├── backend/
│   ├── include/xi/          ← 50+ headers (xi_abi, xi_async, xi_var, …)
│   ├── src/
│   │   ├── service_main.cpp ← xinsp-backend.exe (WS server)
│   │   └── runner_main.cpp  ← xinsp-runner.exe (headless)
│   ├── tests/               ← C++ unit tests (xi_core, record, protocol, …)
│   └── CMakeLists.txt
├── vscode-extension/        ← VS Code integration + Node E2E tests
├── plugins/                 ← shipped plugins
├── sdk/                     ← plugin SDK (scaffold, cmake, template, examples)
└── examples/                ← user-script examples + crash_tests
```

---

## Testing matrix

| Layer                        | Command                                          | What it proves                                              |
|------------------------------|--------------------------------------------------|-------------------------------------------------------------|
| C++ unit                     | `ctest --test-dir backend/build -C Release` (12 targets) | Core types & traits; ImagePool concurrency; Record paths |
| WS protocol                  | `node --test vscode-extension/test/ws_*.test.mjs` | Command surface, crash recovery, fragmentation, adversarial |
| Multi-camera correlation     | `node vscode-extension/test/runMulticam.mjs`     | `synced_stereo` pairs left+right under same tid, 17/17      |
| Record/replay                | `node vscode-extension/test/runRecordReplay.mjs` | Observer records → replay dispatches every event, 11/11     |
| Remote auth                  | `node vscode-extension/test/runRemoteAuth.mjs`   | Bearer accept/deny/401, back-compat, constant-time compare  |
| User journey (full VS Code)  | `node vscode-extension/test/runUserJourney.mjs`  | 10 steps, 24 screenshots, real webview + UI assertions      |
| Headless runner              | `node vscode-extension/test/runHeadlessRunner.mjs` | xinsp-runner.exe produces a correct per-frame JSON report |

---

## Status

See [`docs/roadmap/README.md`](docs/roadmap/README.md) for what's shipping and the
locked-in decision log. Process isolation + SHM were removed 2026-05;
all plugins run in-process.

---

## Documentation index

Pick the entry point closest to your task — every doc tells you the
next jump.

| If you need to… | Read |
|---|---|
| Understand what xInsp2 is | This README |
| See the technical map | [`docs/overview.md`](docs/overview.md) |
| Know what's shipping vs WIP | [`docs/roadmap/README.md`](docs/roadmap/README.md) |
| Run / add tests | [`docs/testing.md`](docs/testing.md) |
| Drive the WebSocket protocol | [`docs/reference/ws-protocol.md`](docs/reference/ws-protocol.md) |
| Add a plugin | [`docs/guides/write-a-plugin.md`](docs/guides/write-a-plugin.md) |
| Write an inspection script | [`docs/guides/write-a-script.md`](docs/guides/write-a-script.md) |
| Debug a crash | [`docs/guides/debug.md`](docs/guides/debug.md) |
| Extend the VS Code UI | [`docs/guides/extend-the-ui.md`](docs/guides/extend-the-ui.md) |
| Look up `host_api` / ABI | [`docs/reference/`](docs/reference/) |
| Browse worked examples | [`sdk/examples/`](sdk/examples/) + [`sdk/README.md`](sdk/README.md) |
| First plugin in 5 minutes | [`sdk/GETTING_STARTED.md`](sdk/GETTING_STARTED.md) |

---

## License

See individual files for vendored library licenses (yyjson — MIT;
stb_image_write — public domain). Everything else is this project's own.
