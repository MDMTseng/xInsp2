# Adding a plugin

A plugin is a C++ DLL that exports a small C ABI (`xi_plugin_create`,
`xi_plugin_destroy`, `xi_plugin_process`, `xi_plugin_exchange`,
`xi_plugin_get_def`, `xi_plugin_set_def`). The backend loads it at
project-open time and the inspection script reaches it via
`xi::use("instance_name")`.

> **Why you're writing a plugin — the project's spine.** xInsp2 is built on three
> non-negotiable principles ([README → Core principles](../../README.md#core-principles--the-spine-do-not-drift),
> the canonical statement): **speed-first, minimal core, functionality-first as
> plugins.** The core is a deliberately *dumb hub* — it holds zero domain knowledge;
> every capability (detectors, cameras, I/O, history, analytics, even orchestration)
> is a plugin composed over the existing ABI. What this means for you as a plugin
> author:
> - **Your plugin owns its domain logic + state** — not the core, not the script.
>   The script just wires; the core just dispatches. Keep the smarts in here.
> - **Respect the hot path** — `process()` runs per frame. Zero-copy (images *and*
>   JSON already move by pointer — don't copy them), no I/O or allocation you can
>   avoid, measure before trading throughput for convenience.
> - **A feature that feels like it needs a core change almost never does.** Reach
>   for plugin composition first; the platform stays minimal precisely so it stays
>   fast and stable. If you truly hit a wall, ask for the *smallest* host primitive
>   — never a feature baked into the core.

xInsp2 supports **two authoring paths** with the same C ABI on the
output side. Pick by audience:

- **In-project** — fastest iteration, plugin lives inside an inspection
  project, hot-rebuild on save. Best for project-specific operators.
- **Standalone** — plugin lives in its own folder + git repo, builds
  out-of-tree, distributable. Best for reusable operators you'll share.

Both produce the same shape of output. You can prototype in-project
then export to standalone when you're ready to share.

> **Before you ship:** skim [`plugin-caveats.md`](./plugin-caveats.md) — the
> non-obvious gotchas (reentrancy/locking, the `prepare`/`commit` staging
> contract, image-handle refcounts, and the UI patterns: keep UX flow in the
> webui not C++, and don't reimplement geometry in JS).

The repo ships reference plugins to crib from: `plugins/` (source/sink/processor
basics — `mock_camera`, `blob_analysis`, `data_output`, `json_source`,
`record_save`, `cache`, `synced_stereo`, `expose`) and richer worked examples under
`examples/*/plugins/`.

---

## Path 1 — in-project (recommended for getting started)

### 1. Open a project in VS Code

If you don't have one:
- Command palette → **xInsp2: Create Project** → pick a folder.

### 2. Generate the plugin scaffold

- Command palette → **xInsp2: New Project Plugin…**
  (or the file-code (📄) action in the **Instances & Params** view's `⋯` menu).
- Pick a starting point. The picker lists three **templates** (rendered from
  `sdk/templates/`) and, under a separator, every **example** under
  `sdk/examples/`:
  | Starting point | What it shows |
  |---|---|
  | **Easy** (template) | Pass-through. ~30 lines, every method has a tutorial comment |
  | **Medium** (template) | Image processor (threshold + binary output) with UI panel + inline image preview (pan / cursor-zoom) |
  | **Expert** (template) | Stateful synthetic source with a worker thread + UI start/stop controls |
  | *From an example* (`sdk/examples/*`) | Copies a worked example (e.g. `comm`, `invert`, `histogram`, `trigger_source`) as your starting code |
- Type a plugin name (lowercase + underscores, e.g. `roi_filter`).

> **From-an-example copies with rename.** Picking an example copies its single
> `.cpp` into `plugins/<name>/<name>.cpp`, renames the C++ class (the
> `XI_PLUGIN_IMPL(...)` class → PascalCase of your name) and the `plugin.json`
> `name`/`dll`, and **preserves the example's manifest flags** (`sink`,
> `reentrant`, `manifest`, …). The example's standalone `CMakeLists.txt` is
> dropped — an in-project source plugin is compiled by the backend
> (`compile: true`), not built standalone.

The extension drops files at `<project>/plugins/<name>/`:

```
plugins/<name>/
├── plugin.json          ← manifest
├── src/plugin.cpp       ← your code
└── ui/index.html        ← (Medium / Expert) plugin UI panel
```

…**and declares it in `project.json`** — plugins are loaded from explicit
declarations, not by scanning the folder (see *How plugins are declared* below).
The command adds `plugins.<name> = { "path": "<name>", "compile": true }` for you,
where `./plugins` is the default search root. The backend then compiles the plugin
in-place with debug-friendly flags (`/Od /Zi /RTC1`), so you can attach a debugger.

### 3. Edit + save = hot reload

`Ctrl+S` on the .cpp triggers a recompile. Output:
- Successful compile → backend reloads the plugin in ~1 second; any
  instances using it survive (their `set_def` state is replayed).
- Compile error → red squiggle on the offending line; Problems panel
  shows the cl.exe error.

### 4. Create an instance

Plugin is the *type*; an instance is a *configured use*. To use the
plugin from your inspection script:

- Click the `+` (Add Instance) button in the **Instances & Params** view
  and pick your plugin. Pick a name (e.g. `det0`).
- Open the instance UI (gear icon next to the instance) → tune fields.
- In your `inspect.cpp`:
  ```cpp
  auto out = xi::use("det0").process(input);
  ```

`xi::use` returns a proxy; `process()`, `exchange()`, `get_def()`,
`set_def()` all forward into the plugin instance.

### 5. Export when ready to share

- Plugin Browser → the **Export** button on your project plugin
  (or command palette → **xInsp2: Export Project Plugin…**) → pick a
  destination folder.
- The extension does a Release rebuild, then copies
  `plugin.json + <name>.dll + <name>.pdb` (and `ui/` if present) into
  `<dest>/<name>/`. That folder is now a standalone plugin you can drop
  into any other project.

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

#### How plugins are declared (`plugin_dirs` + `plugins`)

**Every** plugin a project uses — local OR external — is declared in `project.json`.
There is **no auto-discovery**: a folder under `./plugins` does nothing until it's
listed. This keeps a project self-describing and portable, and lets you pick exactly
which plugins a project loads (handy when you keep a toolbox of many plugins).

```jsonc
{
  // ordered SEARCH ROOTS. Keep these portable: relative paths (resolved against
  // the project folder), ${ENV} expansion, and ~ — NOT absolute machine paths.
  // Falls back to ["./plugins"] when omitted; set it and you get exactly your
  // roots (re-add "./plugins" yourself if you still want the local folder).
  "plugin_dirs": ["./plugins", "${XINSP2_PLUGIN_PATH}", "../shared-plugins"],

  // plugin DECLARATIONS. Each `path` is looked up in each root, first match wins
  // (like $PATH / a makefile search path). `compile` (per-plugin) decides whether
  // the resolved folder is cl.exe-compiled / built + trusted, or just registered.
  "plugins": {
    "myop":    { "path": "myop", "compile": true },   // local: ./plugins/myop, source -> compiled
    "blob":    { "path": "vision/blob_detect" },       // external: prebuilt / build:cmake
    "matcher": { "path": "author/toolbox4/matcher" }
  }
}
```

A **local** plugin is just a declaration whose `path` resolves under `./plugins`
(no `./` needed) — there's nothing special about the project folder. **xInsp2: New
Project Plugin** writes the declaration (with `compile: true`) for you.

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
shows in the Plugin Browser as a project plugin you can recompile / rebuild from VS
Code. It's per-plugin (each entry decides); a project-level
`"plugin_dirs_compile": true` sets the default for entries that omit `compile`.
Off by default because compiling writes a `build/` into the resolved folder — fine
for a toolbox you own, not for a read-only shared registry.

#### Editing the declarations from VS Code — **Plugin Browser**

You don't have to hand-edit `project.json`. **xInsp2: Plugin Browser** (the 🔍
button in the **Instances & Params** view's `⋯` menu, or the command palette) is
the single plugin-management surface — a GUI over exactly the `plugin_dirs` +
`plugins` model above:

- **Added plugins** — every declaration in `plugins`, with its resolved `path`, a
  live loaded ● dot + **×N used** count, a per-plugin **compile** checkbox, and
  **Remove**. Project plugins also get **Rebuild** (cmake) / **Export**, and any
  discovered plugin gets **Reveal** (open its folder in the file explorer).
- **Browse** — one collapsible folder tree per search root in `plugin_dirs` (or
  `./plugins (default)` when none is set). A folder holding a `plugin.json` is a
  plugin node with **Add** (already-declared ones show **✓ added**); nested
  toolbox layouts (`author/toolbox/plugin`) expand too. **Add** writes a `plugins`
  entry, defaulting `compile` to `true` when the folder has `.cpp`/`CMakeLists`
  (source/cmake) and `false` for a prebuilt-only folder. Each root has **Reveal**
  and (when user-added) **Remove**.
- **+ Add folder…** — pick a folder to append to `plugin_dirs` (relativised to
  `./…` when possible so the project stays portable, else an absolute path).

Every action writes `project.json` and reopens the project, so it takes effect
immediately; the backend's save round-trips these declarations, so nothing you set
here gets erased on the next save.

### 4. Iterate

Edit → rebuild DLL → backend hot-reloads. Plugins are trusted and load
straight through — no certification step.

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

If the rebuild **fails to unload** the old module (stale worker thread / GPU
context), it is reported failed and the change-gate is **left unstamped** — so the
*next* Rebuild still sees the plugin as changed and retries the reload, instead of
deciding "unchanged" and running stale code forever.

> A `build: cmake` plugin needs its DLL built before it can load. On first
> `open_project` it shows a "no built DLL — run Rebuild Plugins" warning; run the
> command once and it loads.

> **Manifest flags are re-read on every reload.** `reentrant` (alias `thread_safe`),
> `sink` / `role`, `json_fallback`, `on_fault`, and `factory` are re-parsed from
> `plugin.json` on **all** load paths — full `open_project`, the `Ctrl+S` cl.exe hot-recompile,
> *and* the cmake **Rebuild** — not just the first open. Toggle `"reentrant": true`
> → `false` and Save/Rebuild, and the host immediately starts serializing that
> instance again (the dispatch admission cap follows the live flag); flip a plugin
> to `"sink": true` and its `process()` starts landing in frame order. These flags
> are honoured **only as top-level keys** in `plugin.json` — a `"reentrant":true`
> written inside a nested `manifest` example block or a description string is
> ignored (it does **not** disable serialization).

> **Closing or switching projects frees every plugin the project loaded.** That
> includes compile:false externals resolved from `plugin_dirs` **and** plugins
> whose manifest `name` differs from their folder name — each is unloaded
> (`FreeLibrary`) and dropped from the registry, so the next project's same-named
> plugin loads *its own* DLL rather than reusing a stale handle.

---

## What a plugin looks like (Easy template, abbreviated)

```cpp
class MyPlugin : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& in) override {
        auto src = in.get_image("frame");                 // read-only INPUT view
        auto dst = pool_image(src.width, src.height, 1);  // writable OUTPUT
        cv::GaussianBlur(xi::as_cv_read(src), xi::as_cv_write(dst), {0, 0}, 2.0);
        return xi::Record().image("blurred", dst);
    }
};

XI_PLUGIN_IMPL(MyPlugin)
```

`xi::Plugin` is the base; `XI_PLUGIN_IMPL` generates the C ABI exports.

### Concurrency & config-change safety — which plugin type are you?

The host can call `process()` while the operator is changing your config
(`set_def` / the orchestrator's `prepare`+`commit`). Whether that's safe — and
whether *you* have to do anything about it — depends on two independent choices:
**are you reentrant?** and **do you keep mutable state?** The host has one
admission gate per instance (the "CallScope"): for a **non-reentrant** plugin
(the default) it serializes `process`/`set_def`/`exchange`/`get_def` so they never
overlap; for a **reentrant** plugin (`"reentrant": true` in `plugin.json`, for
per-instance parallelism) it does **not** — concurrency is then your problem.

Find your row — most plugins are T0 or T1 and write **zero** concurrency code:

| Type | You are… | What you must do |
|---|---|---|
| **T0** | reentrant **and** stateless / immutable config (e.g. a pure `gray→blur`) | Nothing. No shared mutable state ⇒ no races even at full parallelism. Fastest. |
| **T1** | non-reentrant with a mutable config (the common case, e.g. a threshold you tune live) | Nothing. The host serializes `set_def` vs `process` for you — a live param change can't tear a frame. |
| **T2** | reentrant **and** you mutate shared state across concurrent `process()` calls (accumulators, caches, counters) | **Lock it yourself.** Reentrancy means N `process()` run at once; the host won't serialize them. This is *your* lock, about process-vs-process — unrelated to config swaps. |
| **T3** | any of the above **plus** a config change that reloads heavy assets you don't want to stall the pipeline | Implement the double-slot `prepare`/`commit` below (`XI_PLUGIN_STAGED`). Orthogonal to T0–T2: it composes with whichever you are. |

**Caveats that bite:**

- **"Reentrant ⇒ must lock" is false.** A *stateless* reentrant plugin (T0) needs
  no lock — that's the cheapest, fastest plugin you can write. The lock (T2) is
  only for shared *mutable* state touched during `process()`.
- **`binarize(threshold)` is NOT stateless.** `threshold` is set by `set_def` and
  read by `process` — that *is* racy mutable state. As non-reentrant (T1) the host
  protects it; as reentrant you'd be T2 and must guard it.
- **The lock-free escape hatch.** If you want T2's throughput *without* a lock:
  put **all** mutable state behind a single `std::atomic<std::shared_ptr<const T>>`
  and make `process()` a pure read of it (swap a fresh immutable snapshot on
  change). Then reads never tear and you need no mutex — this is exactly the T3
  double-slot shape, doing double duty.
- **`prepare` runs UNGATED.** The host calls `prepare()` *concurrent* with
  `process()` on purpose (so the load doesn't stall the pipeline) — so `prepare`
  must touch **only** the staging slot, never live state. That contract is yours
  the moment you add `XI_PLUGIN_STAGED`; `commit()` is the only part the host gates.

### Heavy config changes — frame-perfect swap (optional)

If a config change reloads **big assets** (model weights, templates, calibration)
and you don't want that load to stall the running pipeline, opt into the two-phase
swap: override `prepare()` / `commit()` and add `XI_PLUGIN_STAGED(MyPlugin)` after
`XI_PLUGIN_IMPL`. `prepare(def, folder)` loads the new assets into a **background
staging slot** — the host calls it *concurrent with* `process()`, so it must touch
**only** staging, never live state. `commit()` then **atomically swaps** staging →
live. The canonical lock-free shape: keep the live config in a
`std::atomic<std::shared_ptr<const T>>` that `process()` reads, build a new one in
`prepare()`, swap the pointer in `commit()`. Omit all this and a config change is a
plain (host-serialized) `set_def` — fine for light plugins. The orchestrator drives
it via `prepare_instance` + `commit_group`; see `plugins/config_swap_probe/` for a
complete example and [`../reference/c-abi.md`](../reference/c-abi.md) §1.

### After a caught crash — `on_fault` policy (optional)

The host **catches** a `process()`/`exchange()` fault (an access violation, a
throw) so one bad frame can't take the backend down: the fault is logged, the
instance is marked `degraded` in the health contract (`get_health`), the frame is
dropped, and — by default — **the same instance is reused for the next frame**.
That is exactly right for a **stateless** operator. But if your plugin faults
*midway through mutating persistent state* (a half-updated model, a container
resized but not filled), reusing it means the next frame reads inconsistent state
and can produce a silently-wrong result. `on_fault` lets you choose what happens:

| `on_fault` | After a caught fault | Use it when |
|---|---|---|
| `reuse` *(default)* | logged + `degraded`; instance stays in service | your `process()` holds no cross-frame state that a mid-fault could corrupt (stateless operators — most plugins) |
| `reinit` | the instance is **torn down and re-created + re-prepared from its last committed config** before its next use, dropping in-flight state; after 3 consecutive rebuild failures it escalates to `refuse` | you keep mutable state across frames (accumulators, trackers, a loaded model) whose invariants a mid-fault could break |
| `refuse` | the instance is **pulled from service**: subsequent `process()` calls fail fast (never entering your code) and it shows `failed`/`quarantined` in `get_health`, until an operator re-enables it | a wrong-but-not-crashing result is worse than no result — better to stop the station than pass a bad part |

Declare it as a **per-plugin default** in `plugin.json`:

```json
{ "name": "blob_tracker", "reentrant": false, "on_fault": "reinit" }
```

and override it **per instance** in `instance.json` (`"on_fault": "refuse"`) when
one deployment of the plugin needs a different posture. Absent/unknown ⇒ `reuse`,
so existing plugins are unaffected. Like the other dispatch flags it is honoured
**only as a top-level key** and is re-read on reload.

A `reinit` rebuild rides the same lifecycle as a plugin reload — it re-runs your
`xi_plugin_create` + restores the last committed config, so **anything your
constructor/`set_def` rebuilds is restored, and anything only mutated during
`process()` is dropped** (that's the point). To **re-enable** a `refuse`-d
instance, re-commit its config (`set_instance_def` / `commit_group`) — the same
action an operator takes to push a corrected config. See
[`../new_gen/04-health-contract.md`](../new_gen/04-health-contract.md) and the
`get_health` reason codes in [`../reference/ws-protocol.md`](../reference/ws-protocol.md).

### Image ops

xInsp2 doesn't ship its own operator library — `xi.hpp` /
`xi_plugin_support.hpp` pull in `<opencv2/opencv.hpp>` and plugins call
`cv::*` directly. A small set of helpers make it zero-copy across the ABI while
encoding the **read-only-input / writable-output invariant** in the types:

| Helper | Purpose |
|---|---|
| `xi::as_cv_read(img)` | Non-owning `cv::Mat` view for **READING** an image — pass it as a cv:: **source**. Reads through `Image::read()` (the const, blessed input accessor). Use for every INPUT. |
| `xi::as_cv_write(img)` | Non-owning `cv::Mat` view for **WRITING** a writable OUTPUT — pass it as a cv:: **destination**. Empty Mat (a loud cv:: error, not silent corruption) if `img` is an input view rather than a writable output. |
| `xi::as_cv_mat(img)` | Legacy always-mutable view (kept for existing code). Prefer `as_cv_read` / `as_cv_write` so read-vs-write intent is visible. |
| `Plugin::pool_image(w, h, c)` | Allocate a fresh **writable OUTPUT** slot in the host's ImagePool and return a refcounted Image whose `write()` (and `as_cv_write()`) point straight at pool memory. cv:: writes land in the pool, so returning the Image from `process()` short-circuits to an `addref` — no heap-to-pool memcpy on the way out. |

> **Input is read-only — enforced, not just documented.** A trigger/input image
> (`in.get_image("frame")`) is a zero-copy view over pool memory **aliased across
> consumers**. Writing into it corrupts every other consumer's input. The type
> system now backs the rule: an input Image's blessed accessor is the const
> `read()` (and `as_cv_read`), while `write()` / `as_cv_write` yield a mutable
> pointer **only** for a freshly-created output (`pool_image` / `output_image`) —
> on an input they return null / an empty Mat. At the ABI this is the
> `xi.imaging_rw@1` interface (`image_read` / `image_write`, where `image_write`
> returns null for a shared handle). **Never** do an in-place cv:: op on an input;
> always `as_cv_read(src)` → `as_cv_write(dst)` into a separate `pool_image`.

Don't hand-roll Image⇄Mat copies or the RGB↔BGR flip — `<xi/xi_cv.hpp>` ships the
canonical helpers: `xi::to_cv(img)` (owning copy), `xi::from_cv_mat(mat)` /
`xi::to_image(mat)` (Mat→owning Image), and `xi::encode_preview(img, ".jpg")`
(RGB→BGR + encode to a JPEG/PNG buffer, so preview colours come out right).

Pattern:

```cpp
auto src = input.get_image("src");                       // read-only INPUT view
auto dst = pool_image(src.width, src.height, 1);         // writable OUTPUT sink
cv::GaussianBlur(xi::as_cv_read(src), xi::as_cv_write(dst), {0, 0}, 2.0);
return xi::Record().image("blurred", dst);               // zero-copy across ABI
```

**Pixel order is RGB**, not OpenCV's default BGR. The decoder behind
`xi::imread` is stb_image, which emits RGB-ordered pixels; that order
flows through the host pool unchanged. When you hand a Mat to
`cv::cvtColor`, use `cv::COLOR_RGB2*` (e.g. `RGB2GRAY`, `RGB2HSV`),
**not** `BGR2*`. Mixing them up is a silent failure — red and blue
swap, hue values land 120° away from where they should be, and the
plugin still "works", just on the wrong colour. Corollary:
`cv::imencode` / `cv::imwrite` expect **BGR**, so before encoding an RGB
overlay to a JPEG preview, `cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR)`
first — otherwise the preview's colours are R/B-swapped.

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

### Building a richer output Record

`xi::Record::set` is **not** scalar-only — nest sub-records and build arrays, so a
variable-length result set needs no hand-rolled JSON string:

```cpp
xi::Record out;
out.set("count", n);
out.set("best", xi::Record().set("x", bx).set("y", by).set("score", bs));   // nested object
for (auto& f : features)
    out.push("features", xi::Record()                                        // array of objects
        .set("pose", xi::Record().set("x", f.x).set("y", f.y).set("angle", f.angle))
        .set("score", f.score));
return out;
```

`push(key, …)` appends to (and creates) an array; `set(key, const Record&)` nests
an object (its images are merged in under `"<key>.<imgkey>"`, not dropped);
`set_value(key, Value)` deep-copies any value from another Record (the safe
cross-doc copy verb that replaced the old raw `set_raw` escape hatch). See
[`reference/data-types.md`](../reference/data-types.md).

### Test `process()` headlessly — `xi_run_plugin`

To exercise a plugin's `process()` on an image **without** VS Code / the backend /
a WS connection, build the host-mock CLI once and point it at any plugin DLL:

```bat
set XINSP2_ROOT=C:\path\to\xInsp2
cmake -S %XINSP2_ROOT%\sdk\host_mock -B %XINSP2_ROOT%\sdk\host_mock\build -A x64 -DXINSP2_ROOT=%XINSP2_ROOT%
cmake --build %XINSP2_ROOT%\sdk\host_mock\build --config Release

xi_run_plugin <plugin.dll> --image src=in.png --def "{...}" --out-dir out --runs 1
```

It stands up the real `ImagePool` host_api (so `pool_image` + the image getters
behave as in the backend), creates one instance, optionally `set_def`s a config,
loads each `--image` (OpenCV, BGR→RGB), runs `process()`, and prints the output
Record JSON + writes any output images (RGB→BGR) to `--out-dir`. `--runs N` repeats
for stateful plugins. Great for CI / offline geometry checks of a `build:cmake`
plugin's core.

---

## Plugin lifecycle & threading contract

Every plugin C-ABI export runs in a specific **lifecycle state**, on a specific
**thread**, under a specific **serialization guarantee**, and with a specific
**ambient-context** rule. None of this was written down before — and an unwritten
contract is exactly what produced the parallel-region context bugs (a worker thread
the plugin spawns does *not* inherit the host's ambient `current_trigger` /
image-pool owner). This section **is** that contract, source-verified against
`xi_abi.h` and `xi_cabi_adapter.hpp`.

**The states an instance moves through:**

- **pre-create** — the DLL is loaded and ABI-gated, but no instance object exists yet.
- **live** — the instance exists; the host may call it. Config ops, `process`, and
  the staging swap all happen here.
- **destroyed** — `destroy` has run; no further call is legal.

**The serialization gate (`CallScope`).** The adapter wraps most entry points in a
per-instance admission gate (`xi_cabi_adapter.hpp:293-311`). For a **non-reentrant**
plugin (the default) the gate admits **one** call at a time *across*
`process` / `exchange` / `get_def` / `set_def` / `commit` — so a live config change
can't tear an in-flight `process`. A **reentrant** plugin (`"reentrant": true`)
lifts the gate (up to `max_concurrency`, or unlimited) and owns its own locking.
`prepare` is the one entry that runs **outside** the gate on purpose.

**The ambient-context rule.** While a gated call runs, the adapter holds an
`ImagePool::OwnerGuard` (`xi_cabi_adapter.hpp:212,222,229,244,258,268`) so any image
the plugin allocates via `host->image_create` is tagged to this instance and swept
on destroy. The dispatch worker that runs `process` *also* set the script's
`current_trigger` ambient. **Both are `thread_local`** — they live only on the
thread the host called you on. A worker thread you spawn inside `process`
(`std::thread`, an `xi::async` / OpenMP body) inherits **neither**: images it creates
are tagged `owner=0` (anonymous — outside the per-owner leak sweep) and
`current_trigger()` reads empty there. Read ambient state on the host's thread; hand
captured values into your own threads.

### The contract table

| Export | Legal state | Thread | Serialization | Ambient context |
|---|---|---|---|---|
| `xi_plugin_abi_version` | pre-create (load gate) | control thread, before any instance exists | n/a (pure constant) | none |
| `xi_plugin_create` | pre-create → live | control thread (`create_instance` / `open_project` / rename / rebuild) — never a dispatch worker | implicit: the instance isn't visible to dispatch until `create` returns | `ImagePoolOwnerScope` active — ctor images are owner-tagged & swept (`xi_plugin_manager.hpp:345,1803`). No trigger. |
| `xi_plugin_process` | live | a **dispatch worker** thread (`service_main.cpp:159-163`); for a `sink`, staged & flushed in the ordered-emit gate | **gated** by `CallScope` — serialized per instance **unless** `reentrant=true` (`xi_cabi_adapter.hpp:242-248,286`) | `OwnerGuard(owner_id_)` set by the adapter (`:244`); the worker also holds the script's `current_trigger`. Both `thread_local` — do **not** cross into threads you spawn. |
| `xi_plugin_exchange` | live | the caller's thread — control thread for a UI command (`service_main.cpp:262,2876`), or the inspect thread for a script `xi::use().exchange()` | **gated** by `CallScope` (`xi_cabi_adapter.hpp:227-234`) | `OwnerGuard(owner_id_)` (`:229`). No trigger guarantee. |
| `xi_plugin_get_def` | live | control thread (project save) | **gated** by `CallScope` (`xi_cabi_adapter.hpp:210-217`) | `OwnerGuard(owner_id_)` (`:212`). |
| `xi_plugin_set_def` | live | control thread (load / `set_instance_def` — `service_main.cpp:2804,3214`) | **gated** by `CallScope` (`xi_cabi_adapter.hpp:220-224`) — serialized vs `process` | `OwnerGuard(owner_id_)` (`:222`). |
| `xi_plugin_prepare` (v7, opt) | live (background) | control thread (`prepare_instance` — `service_main.cpp:2953`) | **UNGATED — no `CallScope`** — runs **concurrent with `process`** so the load never stalls the pipeline (`xi_cabi_adapter.hpp:250-260`; `xi_abi.h:107-119`) | `OwnerGuard(owner_id_)` only (`:258`). **Contract: touch the staging slot ONLY, never live state.** |
| `xi_plugin_commit` (v7, opt) | live | control thread (`commit` / `commit_group` — `service_main.cpp:3046`) | **gated** by `CallScope` (`xi_cabi_adapter.hpp:262-270`); under `commit_group`, dispatch is drained first, so uncontended | `OwnerGuard(owner_id_)` (`:268`). |
| `xi_plugin_destroy` | live → destroyed | control thread (`remove_instance` / close / rebuild / shutdown), in `~CAbiInstanceAdapter` (`xi_cabi_adapter.hpp:177-194`) | **not** gated, but the host removes the instance from dispatch first, so no `process` can be in flight | none around `destroy_fn`; the dtor then runs `release_all_for(owner_id_)` to sweep leaked images (`:188`). |

> "Control thread" = whichever backend thread is servicing the WS command (or
> `open_project` / shutdown). The point that matters is the **gate**, not the exact
> thread: `process` is the only export that runs on a dispatch worker, and config
> ops are serialized against it for a non-reentrant instance.

> The two v7 exports (`prepare` / `commit`) exist only if the plugin opted in with
> `XI_PLUGIN_STAGED`. Without them, a heavy config change is a plain gated `set_def`
> (see *Heavy config changes* above). The asymmetry is the whole point:
> **`prepare` is ungated and concurrent with `process`; `commit` is gated.** That is
> sound only because `prepare` touches the staging slot alone.

> **Debug builds machine-check this contract (G3.2).** The one transition the gate
> *cannot* make safe is a **same-thread re-entry** into a non-reentrant instance's
> own gated export — a plugin's `process()` body calling back into the host to run
> `set_def()` / `commit()` / `process()` on **its own instance**. `CallScope`
> (cap = 1) would then wait on a slot this very thread already holds → a silent
> **deadlock**. In `Debug` (behind `#ifndef NDEBUG`, compiled to nothing in
> Release) the adapter detects that re-entry and raises a **loud, catchable**
> lifecycle-contract violation (aborts by default; tests swap in a recorder) instead
> of hanging. The two named illegal cases are *"`set_def()` during `process()`"* and
> *"out-of-order lifecycle (`process` before `commit`)"*. A `reentrant: true`
> instance lifts the gate and owns its locking, so its re-entry is **not** flagged.
> Don't call the host back into your own non-reentrant instance from inside a gated
> export; do the work directly or declare the plugin reentrant.

### Parallelism invariants (for plugin authors)

These hold regardless of plugin type (T0–T3) and mirror the script-side rules in
[`write-a-script.md`](./write-a-script.md):

1. **Ambient context is `thread_local` and lives on the host's call thread.** A
   thread you spawn inside `process` (or any export) does **not** inherit
   `current_trigger` or the image-pool owner. Read what you need on the host thread
   and capture it by value into the worker.
2. **A pool image created on a worker thread you spawn is tagged `owner=0`**
   (anonymous) — thread-safe and fine on the happy path (balanced refcounts), but
   **outside** the per-owner leak sweep that runs on `destroy`. To keep it
   attributed, allocate on the host's call thread, or set an `ImagePool::OwnerGuard`
   in the worker (`xi_image_pool.hpp:137,226`).
3. **A C++ exception must not cross a `#pragma omp` region boundary** — OpenMP
   requires it caught inside the same structured block. Catch inside, set a flag,
   rethrow on the call thread.
4. **`process` may run concurrently with `prepare`.** If you exported
   `XI_PLUGIN_STAGED`, `prepare` runs with no gate while `process` is in flight — so
   `prepare` must write only the staging slot and `process` must read only live
   state. The canonical lock-free shape (atomic `shared_ptr<const Config>`) gives you
   this for free.
5. **Reentrancy is about `process`-vs-`process`, not config swaps.** The `CallScope`
   gate already serializes config ops (`set_def` / `commit`) against `process` for a
   non-reentrant instance; opting into `reentrant=true` is what makes N `process`
   calls overlap, and then shared mutable state is your responsibility.

(These restate, plugin-author-side, the host invariants in
[`../internals/core_fix_plan.md`](../internals/core_fix_plan.md) Part I §6 and
Part III §20.)

---

## Common questions

**Where does instance state persist?**
Each instance gets `<project>/instances/<name>/`. The host calls
`get_def()` on save, hands the JSON to `set_def()` on load. For larger
data (calibration images, ML weights), write to that folder using
`host->instance_folder()` to get the path.

**What's the on-disk instance shape, and how are instances loaded?**
`<project>/instances/<name>/instance.json` is `{ "plugin": "<plugin-name>",
"config": { … } }`. On `open_project` the backend **auto-discovers** every
`instances/*` folder and creates the instance; its `config` object is passed
**verbatim as the `set_def` JSON root** (so design `set_def` to parse exactly what
you put under `config`). (Instance auto-discovery from `instances/` is unrelated
to plugins, which are declared explicitly — see *How plugins are declared* above.)
Full lifecycle: [`reference/instances.md`](../reference/instances.md).

**How do I show a UI?**
Set `"has_ui": true` in `plugin.json`, ship a `ui/index.html`. It runs
in a webview; talk to your plugin via
`vscode.postMessage({ type: 'exchange', cmd: <jsonStr> })`. The extension
forwards to your plugin's `exchange()` and posts the response back as
`{ type: 'status', ... }`.

**How do I emit images (camera / source)?**
A source is an ordinary plugin: build an `xi::Record` carrying the frame (and any
routing/context metadata — a command id, recipe, lane hint — the script needs)
and hand it to the host with the one dispatch verb, `xi::emit_record` (ABI v6),
from a worker thread. Each record dispatches one inspection. See the Expert
template for a working synthetic source.

```cpp
auto rec = xi::Record()
    .image("frame", img)
    .set("command", "inspect_top")     // ← rides along as metadata
    .set("recipe", 7);
xi::emit_record(host(), name().c_str(), rec);   // id auto, ts = now
// or, the member sibling that fills host()/name() for you:
emit(rec);                                       // == the line above
```

The `emit(rec)` member (sibling of the free `xi::emit_record`, and of the
output-verb member `emit_binary`) forwards to the free function with the
plugin's own `host()`/`name()` — same staging, same optional `id`/`ts`
defaults — so a source can't pass the wrong emitter name. The free function
stays for out-of-class callers.

The metadata travels with the frame and the script reads it back with
`xi::current_trigger().meta()` — no side-channel queue. It's handed over by
pointer (zero-serialize). Multi-camera capture is a "gathering" plugin that emits
one record carrying N named images. See
[`docs/internals/dispatch.md`](../internals/dispatch.md).

All plugins run in-process (cameras included), so `emit_record` always reaches the
real backend dispatcher — no special config is needed for source plugins. (A
legacy `"isolation"` field in `instance.json` is accepted but ignored with a
one-time deprecation warning; see
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
isolation, which xInsp2 doesn't provide. `examples/dll_version_clash/` is a
runnable demo of all three cases (full-path load, by-name clash, distinct-name
fix).

**My plugin won't load.** Check:
1. Backend stderr — usually says exactly which symbol failed to
   resolve.
2. Plugin tree origin badge — `[project]` means in-project,
   `[global]` means scanned from a plugins dir; mismatched expectations
   often surface here.


---

## Instance UI conventions

A plugin's instance UI is plain HTML the plugin ships in `ui/index.html`, loaded
into a VS Code webview (see [`extend-the-ui.md`](./extend-the-ui.md) for the
host wiring; the `exchange()` contract is covered above).

> **Building the UI:** the [`xi-components`](./plugin-ui.md) web-component library
> gives you a slider/viewer/teach-editor/dashboard toolkit + runnable playground
> pages — start at [`plugin-ui.md`](./plugin-ui.md). The conventions below keep
> whatever UI you build **automatable**.

These conventions keep that UI **automatable** — by the
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

Plugins that ship **source but no built DLL** (all the `examples/` plugins)
can't be instantiated via the scan path — there's no DLL to load. Call
**`h.useProjectPlugin(projectFolder)`** right after `createProject`: it copies
the plugin's source into `<project>/plugins/<name>/` and reopens the project, so
the backend compiles it as a project plugin. This is the supported way to
UI-test a source-only example plugin.
