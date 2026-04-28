# Adding a plugin

A plugin is a C++ DLL that implements a small C ABI (`xi_plugin_create`,
`process`, `exchange`, `get_def`, `set_def`, `xi_plugin_destroy`). The
backend loads it at project-open time and the inspection script reaches
it via `xi::use<T>("instance_name")`.

xInsp2 supports **two authoring paths** with the same C ABI on the
output side. Pick by audience:

- **In-project** — fastest iteration, plugin lives inside an inspection
  project, hot-rebuild on save. Best for project-specific operators.
- **Standalone** — plugin lives in its own folder + git repo, builds
  out-of-tree, distributable. Best for reusable operators you'll share.

Both produce the same shape of output. You can prototype in-project
then export to standalone when you're ready to share.

---

## Path 1 — in-project (recommended for getting started)

### 1. Open a project in VS Code

If you don't have one:
- Command palette → **xInsp2: Create Project** → pick a folder.

### 2. Generate the plugin scaffold

- Command palette → **xInsp2: New Project Plugin…**
  (or click the file-code (📄) icon in the Plugins tree title bar).
- Pick a template:
  | Template | What it shows |
  |---|---|
  | **Easy** | Pass-through. ~30 lines, every method has a tutorial comment |
  | **Medium** | Image processor (threshold + binary output) with UI panel + inline image preview (pan / cursor-zoom) |
  | **Expert** | Stateful synthetic source with a worker thread + UI start/stop controls |
- Type a plugin name (lowercase + underscores, e.g. `roi_filter`).

The extension drops files at `<project>/plugins/<name>/`:

```
plugins/<name>/
├── plugin.json          ← manifest
├── src/plugin.cpp       ← your code
└── ui/index.html        ← (Medium / Expert) plugin UI panel
```

The backend compiles the plugin in-place with debug-friendly flags
(`/Od /Zi /RTC1`), so you can attach a debugger.

### 3. Edit + save = hot reload

`Ctrl+S` on the .cpp triggers a recompile. Output:
- Successful compile → backend reloads the plugin in ~1 second; any
  instances using it survive (their `set_def` state is replayed).
- Compile error → red squiggle on the offending line; Problems panel
  shows the cl.exe error.

### 4. Create an instance

Plugin is the *type*; an instance is a *configured use*. To use the
plugin from your inspection script:

- Plugins tree → right-click your plugin → **Create Instance** (or use
  the `+` button in the Instances tree). Pick a name (e.g. `det0`).
- Open the instance UI (gear icon next to the instance) → tune fields.
- In your `inspection.cpp`:
  ```cpp
  auto out = xi::use<MyClass>("det0").process(input);
  ```

`xi::use` returns a proxy; `process()`, `exchange()`, `get_def()`,
`set_def()` all forward into the plugin instance.

### 5. Export when ready to share

- Plugins tree → right-click your project plugin → **Export Project
  Plugin…** → pick a destination folder.
- The extension does a Release rebuild, runs the baseline cert tests,
  then copies `plugin.json + <name>.dll + <name>.pdb + cert.json` (and
  `ui/` if present) into `<dest>/<name>/`. That folder is now a
  standalone plugin you can drop into any other project.

---

## Path 2 — standalone

When you want a plugin with its own repo + CI / distributable to
others.

### 1. Scaffold

```bat
node <xinsp2>\sdk\scaffold.mjs C:\dev\my_plugins\foo
```

(For the unified `--template easy|medium|expert` flag, use the
`shm-process-isolation` branch's scaffold which renders from
`sdk/templates/`.)

### 2. Build

```bat
set XINSP2_ROOT=C:\dev\xInsp2
cmake -S C:\dev\my_plugins\foo -B C:\dev\my_plugins\foo\build -A x64
cmake --build C:\dev\my_plugins\foo\build --config Release
```

### 3. Tell xInsp2 to load it

One of:
- VS Code setting `xinsp2.extraPluginDirs = ["C:\\dev\\my_plugins"]`
- CLI flag `xinsp-backend.exe --plugins-dir=C:\dev\my_plugins`

The backend's plugin scanner picks up any subfolder with a
`plugin.json`.

### 4. Iterate

Edit → rebuild DLL → backend hot-reloads. The certify step runs once
per DLL hash; cached in `cert.json` next to the DLL.

---

## What a plugin looks like (Easy template, abbreviated)

```cpp
class MyPlugin : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& in) override {
        auto src = in.get_image("frame");
        auto dst = pool_image(src.width, src.height, 1);
        cv::GaussianBlur(src.as_cv_mat(), dst.as_cv_mat(), {0, 0}, 2.0);
        return xi::Record().image("blurred", dst);
    }
};

XI_PLUGIN_IMPL(MyPlugin)
```

`xi::Plugin` is the base; `XI_PLUGIN_IMPL` generates the C ABI exports.

### Image ops

xInsp2 doesn't ship its own operator library — `xi.hpp` /
`xi_plugin_support.hpp` pull in `<opencv2/opencv.hpp>` and plugins call
`cv::*` directly. Two helpers make it zero-copy across the ABI:

| Helper | Purpose |
|---|---|
| `xi::Image::as_cv_mat()` | Non-owning `cv::Mat` view over the same bytes — no allocation, no copy. The Mat must not outlive the Image. |
| `Plugin::pool_image(w, h, c)` | Allocate a fresh slot in the host's ImagePool and return a refcounted Image whose `data()` (and `as_cv_mat()`) point straight at pool memory. cv:: writes land in the pool, so returning the Image from `process()` short-circuits to an `addref` — no heap-to-pool memcpy on the way out. |

Pattern:

```cpp
auto src = input.get_image("src");                       // pool-backed view
auto dst = pool_image(src.width, src.height, 1);         // pool-backed sink
cv::GaussianBlur(src.as_cv_mat(), dst.as_cv_mat(), {0, 0}, 2.0);
return xi::Record().image("blurred", dst);               // zero-copy across ABI
```

**Pixel order is RGB**, not OpenCV's default BGR. The decoder behind
`xi::imread` is stb_image, which emits RGB-ordered pixels; that order
flows through the host pool unchanged. When you hand a Mat to
`cv::cvtColor`, use `cv::COLOR_RGB2*` (e.g. `RGB2GRAY`, `RGB2HSV`),
**not** `BGR2*`. Mixing them up is a silent failure — red and blue
swap, hue values land 120° away from where they should be, and the
plugin still "works", just on the wrong colour.

For the API contracts in detail, see
[`docs/reference/plugin-abi.md`](../reference/plugin-abi.md) and
[`docs/reference/host_api.md`](../reference/host_api.md).

---

## Common questions

**Where does instance state persist?**
Each instance gets `<project>/instances/<name>/`. The host calls
`get_def()` on save, hands the JSON to `set_def()` on load. For larger
data (calibration images, ML weights), write to that folder using
`host->instance_folder()` to get the path.

**How do I show a UI?**
Set `"has_ui": true` in `plugin.json`, ship a `ui/index.html`. It runs
in a webview; talk to your plugin via
`vscode.postMessage({ type: 'exchange', cmd: <jsonStr> })`. The extension
forwards to your plugin's `exchange()` and posts the response back as
`{ type: 'status', ... }`.

**How do I emit images (camera / source)?**
Call `host->emit_trigger(name, tid, ts, images, count)` from a worker
thread. The backend's TriggerBus correlates by `tid`. See the Expert
template for a working synthetic source.

**Crash isolation?**
Same-process plugins are protected by `_set_se_translator`: a segfault
in `process()` becomes an exception and the backend stays up. For
deeper isolation (separate process), see the
`shm-process-isolation` spike on its branch — `instance.json` gains
`"isolation": "process"` opt-in.

**My plugin won't load.** Check:
1. Backend stderr — usually says exactly which symbol failed to
   resolve.
2. `cert.json` next to the DLL — if cert failed, the backend refuses
   to instantiate. Re-cert after fixing the baseline test.
3. Plugin tree origin badge — `[project]` means in-project,
   `[global]` means scanned from a plugins dir; mismatched expectations
   often surface here.
