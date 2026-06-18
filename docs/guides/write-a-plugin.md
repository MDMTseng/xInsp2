# Adding a plugin

A plugin is a C++ DLL that exports a small C ABI (`xi_plugin_create`,
`xi_plugin_destroy`, `xi_plugin_process`, `xi_plugin_exchange`,
`xi_plugin_get_def`, `xi_plugin_set_def`). The backend loads it at
project-open time and the inspection script reaches it via
`xi::use("instance_name")`.

xInsp2 supports **two authoring paths** with the same C ABI on the
output side. Pick by audience:

- **In-project** — fastest iteration, plugin lives inside an inspection
  project, hot-rebuild on save. Best for project-specific operators.
- **Standalone** — plugin lives in its own folder + git repo, builds
  out-of-tree, distributable. Best for reusable operators you'll share.

Both produce the same shape of output. You can prototype in-project
then export to standalone when you're ready to share.

The repo ships reference plugins to crib from: `plugins/` (source/sink/processor
basics — `mock_camera`, `blob_analysis`, `data_output`, `json_source`,
`record_save`, `threshold_op`, `synced_stereo`) and richer worked examples under
`examples/*/plugins/`.

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
- In your `inspect.cpp`:
  ```cpp
  auto out = xi::use("det0").process(input);
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

(`scaffold.mjs` also accepts `--template easy|medium|expert` (default easy),
rendering from `sdk/templates/`.)

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
- **`project.json` `plugin_dirs` + `plugins`** — see below (the portable, in-project way)

The backend's plugin scanner picks up any subfolder with a
`plugin.json`.

#### Referencing external plugins from `project.json` (portable)

`extraPluginDirs` / `--plugins-dir` are machine-level. To keep a project
**self-describing and portable** — so it resolves the same after a fresh clone on
another machine — declare its external plugins in `project.json`:

```jsonc
{
  // ordered SEARCH ROOTS. Keep these portable: relative paths (resolved against
  // the project folder), ${ENV} expansion, and ~ — NOT absolute machine paths.
  "plugin_dirs": ["./plugins", "${XINSP2_PLUGIN_PATH}", "../shared-plugins"],

  // plugin COORDINATES. Each path is looked up in each root, first match wins
  // (like $PATH / a makefile search path). Resolved folders are registered just
  // like a scanned plugin.
  "plugins": {
    "blob":    { "path": "vision/blob_detect" },
    "matcher": { "path": "author/toolbox4/matcher" }
  }
}
```

The split is the point: **coordinates** (`plugins`) are committed and portable;
the **machine-specific** absolute location lives in an env var (`XINSP2_PLUGIN_PATH`)
or a relative checkout layout — never hard-coded in the committed file. On a new
machine: clone the project + the plugin repos, set one env var (or keep them in a
consistent relative location), and it resolves. An unresolved coordinate is
surfaced as an open warning listing every root searched. Resolution runs in the
backend (so headless / FE runs work too); project-local `plugins/<name>` of the
same name win over resolved ones.

By default a resolved plugin is only **registered** — it must already be built
(prebuilt DLL) or be `build: cmake`. To develop a **toolbox of source plugins**
out-of-tree and have them compile + hot-reload like in-project ones, set
`"compile": true` **per plugin**:

```jsonc
{ "plugin_dirs": ["../my-plugin-toolbox"],
  "plugins": {
    "edge": { "path": "edge_detect", "compile": true },   // WIP source — compile it
    "blob": { "path": "blob" }                             // prebuilt / build:cmake — register only
  } }
```

A `compile: true` entry's resolved folder is treated exactly like a
`<project>/plugins/` one: a source plugin is `cl.exe`-compiled, a `build: cmake`
one is built by **Rebuild Plugins**, and either way it's **trusted** (no cert) and
shows in the Plugins tree as a project plugin you can recompile / rebuild from VS
Code. It's per-plugin (each entry decides); a project-level
`"plugin_dirs_compile": true` sets the default for entries that omit `compile`.
Off by default because compiling writes a `build/` into the resolved folder — fine
for a toolbox you own, not for a read-only shared registry.

### 4. Iterate

Edit → rebuild DLL → backend hot-reloads. The certify step runs once
per DLL hash; cached in `cert.json` next to the DLL.

---

## External libraries & CUDA (cmake plugins)

The in-project path compiles your `.cpp` with `cl.exe` and auto-supplies only
xInsp2 + OpenCV + the plugin's own `include/`. To link a **third-party import lib**
or build **CUDA** (`.cu` → `nvcc`), the plugin needs to own its build. Declare that
in `plugin.json`:

```jsonc
{ "name": "my_cuda_op", "build": "cmake" }   // alias: "prebuilt": true
```

A `build: cmake` plugin is **never** compiled by the backend. It can live right
inside `<project>/plugins/<name>/` (so you keep the instance UI + script wiring +
`xi::use()` of other plugins) **and** carry its own `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_cuda_op CXX C CUDA)              # add CUDA here, or enable_language(CUDA)
set(CMAKE_CXX_STANDARD 20)
include($ENV{XINSP2_ROOT}/sdk/cmake/xinsp2_plugin.cmake)

xinsp2_add_plugin(my_cuda_op plugin.cpp kernels.cu)   # creates the DLL target
# ...then any standard CMake on the `my_cuda_op` target:
find_package(CUDAToolkit REQUIRED)
target_link_libraries(my_cuda_op PRIVATE CUDA::cudart)
target_link_libraries(my_cuda_op PRIVATE C:/sdk/lib/foo.lib)   # any external lib
target_include_directories(my_cuda_op PRIVATE C:/sdk/include)
```

The output DLL is named `<name>.dll` (next to the CMakeLists, the
`xinsp2_add_plugin` default). **Ship its dependency DLLs** (`cudart64_*.dll`, your
SDK's `.dll`) **next to that DLL** — the backend loads plugins with
`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`, which searches the plugin's own folder (plus
the app dir + System32) but deliberately **not** `PATH`/CWD (see *Can I ship extra
dependency DLLs* below).

### The build / reload loop — `xInsp2: Rebuild Plugins`

You don't run cmake by hand. The **xInsp2: Rebuild Plugins** command (and the
backend `rebuild_plugins` WS cmd) does, for every `build: cmake` plugin:

1. **Skips** plugins whose sources are all older than their built DLL — so a CUDA
   plugin you didn't touch keeps its GPU context (nothing is reloaded needlessly).
2. For a changed plugin: **unloads** it first (this is mandatory — Windows can't
   overwrite a *loaded* DLL, and cmake emits a fixed-name DLL), snapshotting each
   instance's saved state.
3. Runs the plugin's **cmake** build (configures `build/` on first run).
4. **Loads** the rebuilt DLL and re-creates the instances with their state restored.

If a plugin's worker thread or CUDA context isn't torn down cleanly in its
destructor, the old DLL won't actually unload — the command **reports that plugin
as failed** ("DLL did not unload … NEW code is NOT active") rather than silently
running stale code. Clean up threads / `cudaDeviceReset()` in your destructor.

> A `build: cmake` plugin needs its DLL built before it can load. On first
> `open_project` it shows a "no built DLL — run Rebuild Plugins" warning; run the
> command once and it loads.

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

**You don't manage refcounts yourself.** `pool_image()` returns an
`xi::Image` that holds the pool handle's ref via its internal
`shared_ptr`. Storing it in a local, returning it through
`xi::Record::image("key", img)`, copy-constructing — all the obvious
C++ patterns do the right thing. The cross-ABI return path picks the
handle up with one `addref` and the local `xi::Image` releases its
ref when it goes out of scope. Net refcount: 1, owned by the receiver.
You never call `image_addref` / `image_release` from plugin code.

For the API contracts in detail, see
[`docs/reference/c-abi.md`](../reference/c-abi.md).

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

All plugins run in-process (cameras included), so `emit_trigger`
always reaches the real backend TriggerBus — no special config is
needed for source plugins. (A legacy `"isolation"` field in
`instance.json` is accepted but ignored with a one-time deprecation
warning; see
[`docs/reference/instances.md`](../reference/instances.md).)

**Crash isolation?**
All plugins run in-process — a plugin crash CAN take the backend down
with it. The protections are:

1. An SEH wrapper around every `process()` call. An access violation,
   null deref, divide-by-zero, etc. inside the plugin is caught and
   surfaced as a per-call error — the backend keeps running.
2. What SEH can't catch (stack overflow, heap corruption) crashes the
   backend; the extension auto-respawns it and a crash report +
   minidump is written for diagnosis. See
   [`debug.md`](./debug.md).

Process isolation + SHM were removed 2026-05; crash diagnosability
(minidumps + per-thread breadcrumbs + PDB symbolication) is the
replacement safety net.

**Can I ship extra dependency DLLs with my plugin?**
Yes — drop them inside the plugin folder, next to `<name>.dll`. The backend
loads plugins with `LoadLibraryEx(..., LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)`, so a
plugin's own folder is searched for its dependency DLLs (the backend's app dir —
where OpenCV/turbojpeg/IPP live — and System32 are still searched; CWD and PATH
are not). **But** because all plugins share one process, Windows keeps a single
module per DLL *base name* — and that bites dependencies resolved **by name**
(a normal static import / linking `foo.lib`): two plugins needing different
versions of the same `foo.dll` collide, first one wins, the other silently runs
the wrong version. (A dependency you load yourself **by absolute path** keys on
the full path and does *not* clash.) Fix: give static deps distinct file names
(and link each plugin against its own name), static-link, or pin a shared
version. Same-name side-by-side versioning of a static import would need process
isolation, which xInsp2 doesn't provide. The `examples/dll_version_clash`
experiment proves all three cases; the generated plugin README summarises them.

**My plugin won't load.** Check:
1. Backend stderr — usually says exactly which symbol failed to
   resolve.
2. `cert.json` next to the DLL — if cert failed, the backend refuses
   to instantiate. Re-cert after fixing the baseline test.
3. Plugin tree origin badge — `[project]` means in-project,
   `[global]` means scanned from a plugins dir; mismatched expectations
   often surface here.


---

## Instance UI conventions

A plugin's instance UI is plain HTML the plugin ships in `ui/index.html`, loaded
into a VS Code webview (see [`extend-the-ui.md`](./extend-the-ui.md) for the
host wiring; the `exchange()` contract is covered above). These conventions keep that UI **automatable** — by the
plugin UI test harness today, and by any generic param-tuning tooling later.

### `data-param` / `data-action` — stable, name-keyed selectors

Element `id`s are author-chosen and arbitrary (`#thr`, `#close`), so a test or a
generic tool can't find "the control for param `threshold`" without reading the
HTML. Tag each control with the **canonical name** instead:

- **`data-param="<param_name>"`** on each input that edits a parameter. The value
  must match the param's name in the plugin's `exchange()` status JSON (e.g.
  `data-param="close_radius"`, not the `#close` id).
- **`data-action="<action>"`** on each button (`apply`, `reset`, `start`, `stop`).

```html
<input type="range" id="close" data-param="close_radius" min="0" max="12">
<button id="apply" data-action="apply">Apply</button>
<button id="reset" data-action="reset">Reset</button>
```

Keep your `id`s — `data-param`/`data-action` sit alongside them and don't change
your own JS. The new-plugin scaffolds (`sdk/templates/{medium,expert}`) already
emit these; `examples/circle_counting/plugins/region_counter` is a worked example.

#### Why

It decouples *selectors* from *ids*, makes the manifest param list the single
source of truth a test can enumerate, and lets a generic harness drive **any**
plugin's UI without per-plugin HTML spelunking:

```js
// helpers.cjs — resolve by param name, not id:
h.setParam('inst0', 'close_radius', 8);   // → [data-param="close_radius"]
h.action('inst0', 'apply');               // → [data-action="apply"]
```

(`h.setInput('inst0', '#close', 8)` / `h.click('inst0', '#apply')` still work for id-targeting when
you need it.)

### Driving the UI in a test

Plugin UI tests live in `<plugin>/tests/test_ui.cjs` and run via
`node sdk/testing/run_ui_test.mjs <plugin-folder>` (cold-starts VS Code) or the
**xInsp2: Run Plugin UI Tests** command. The `h` helper drives the real webview
DOM and can screenshot:

```js
module.exports = { async run(h) {
    const proj = h.tmp();
    await h.createProject(proj, 'demo');
    await h.useProjectPlugin(proj);          // compile this plugin from source (see below)
    await h.addInstance('inst0', 'my_plugin');
    await h.openUI('inst0', 'my_plugin');
    h.shot('opened');                        // → tests/screenshots/NN_opened.png
    h.setParam('inst0', 'threshold', 200);
    h.action('inst0', 'apply');
    await h.sleep(150);
    await h.getStatus('inst0');
    h.expectEq(h.lastStatus.threshold, 200, 'threshold round-trips through the UI');
}};
```

### Instantiating an example/source-only plugin: `useProjectPlugin`

Plugins that ship **source but no built+certified DLL** (all the `examples/`
plugins) can't be instantiated via the scan path — the backend's cert gate
rejects them and `create_instance` fails (the error now says so explicitly).
Call **`h.useProjectPlugin(projectFolder)`** right after `createProject`: it
copies the plugin's source into `<project>/plugins/<name>/` and reopens the
project, so the backend compiles it as a **trusted project plugin** (project
plugins are compiled from source and skip the cert gate). This is the supported
way to UI-test an uncertified example plugin.
