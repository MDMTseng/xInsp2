# Getting started

Build your first xInsp2 plugin from zero in about 5 minutes.

> **The five-minute path is _in-project_ authoring.** The recommended way
> to start is to create a plugin **inside a project**: its source lives under
> `<project>/plugins/<name>/`, and the project declares it explicitly in
> `project.json`. Project plugins are **trusted** — there is no certification
> step to clear before your first run. The standalone / external-folder
> workflow (scaffold a plugin in a sibling folder and load it globally) is a
> more advanced setup; it lives at the end of this doc under
> [Advanced: standalone plugins](#advanced-standalone-plugins-external-folder).

---

## Prerequisites

- A cloned xInsp2 repo (you're inside it now) with the backend built —
  you need `xinsp-backend.exe` to load and run anything.
- CMake ≥ 3.16
- A C++ compiler (MSVC 2019+ on Windows, gcc/clang on Linux)
- Node.js (only needed for the scaffold tool and UI tests)
- Optional: VS Code with the xInsp2 extension built/loaded

That's it — no npm dependencies for the plugin itself. Project plugins are
trusted on load: no certification is required for the first-run path.

---

## The project model in one minute

A **project** is a folder with a `project.json`. It declares, explicitly:

- **`instances`** — the configured uses of plugins your script calls by name
  (e.g. an instance named `expose` of the `expose` plugin).
- **`plugins`** — the project-local plugins to build, each a `{ "path", "compile" }`
  entry. Source lives under `<project>/plugins/<path>/`.

```jsonc
{
  "name": "my_project",
  "script": "inspect.cpp",
  "instances": [
    { "name": "cam0",   "plugin": "my_source" },
    { "name": "expose", "plugin": "expose" }
  ],
  "plugins": {
    "my_source": { "path": "my_source", "compile": true }
  }
}
```

There is **no folder auto-discovery for project use** — a project plugin is
used because it is *declared here*, not because it happens to sit in a scanned
directory. (Shipped plugins like `expose` are always available and just need an
`instances` entry.) The standalone parent-folder scan described at the end of
this doc is a separate, global mechanism.

---

## Advanced: standalone plugins (external folder)

> The steps below (1–4) are the **standalone** workflow: a plugin authored in
> its own folder outside any project and loaded **globally** into the host via a
> parent-folder scan. This is optional and more advanced than the in-project
> path above — reach for it when you want one plugin available across many
> projects. For your first plugin, prefer in-project authoring (declare it in
> `project.json`); come back here when you outgrow that.

### 1. Pick a folder for your plugins

It can be anywhere — your home dir, a sibling of the xInsp2 checkout,
inside a different repo. A standalone plugin lives outside any single project.

The recommended layout is to keep xInsp2 and your plugins as siblings
so cmake's auto-detect just works:

```
C:\dev\
├── xInsp2\          ← cloned this repo here
└── my_plugins\      ← your plugins live here
```

```bat
mkdir C:\dev\my_plugins
cd    C:\dev\my_plugins
```

If you put them somewhere else, set the env var once:

```bat
set XINSP2_ROOT=C:\path\to\xInsp2
```

---

### 2. Scaffold a new plugin

The shell wrapper is the shortest path:

```bash
sh /c/dev/xInsp2/sdk/create_plugin.sh my_first_plugin
```

(Use `sh` from git-bash / WSL on Windows, or natively on Linux/macOS.)

If you don't have `sh`, the underlying tool works directly with Node:

```bat
node C:\dev\xInsp2\sdk\scaffold.mjs C:\dev\my_plugins\my_first_plugin
```

You now have a complete plugin folder:

```
my_first_plugin/
├── plugin.json            ← manifest
├── my_first_plugin.cpp    ← thresholder demo, 5 patterns labelled
├── ui/index.html          ← matching webview
├── CMakeLists.txt         ← 3 functional lines
├── tests/
│   ├── test_native.cpp    ← 2 starter XI_TEST blocks
│   └── test_ui.cjs        ← UI E2E test (drives the live webview)
└── README.md              ← 500-line offline kit, plugin-specific
```

The `README.md` inside the new folder is the **complete reference** —
API surface, Record/Json/Image cheatsheets, lifecycle diagram, UI
protocol diagram, common patterns, pitfalls. Open it.

---

### 3. Build it

```bat
cd my_first_plugin
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output: `my_first_plugin.dll` next to `plugin.json`. Also
`my_first_plugin_test.exe` for the native test.

The CMakeLists auto-detects `XINSP2_ROOT` (env var, then sibling check
walking up the tree). No setup beyond the env var.

---

### 4. Load it into xInsp2

Pick one — all three accomplish the same thing:

**VS Code (recommended for dev):**
Settings → search `xinsp2.extraPluginDirs` → add `C:\dev\my_plugins`
(the **parent** folder, not the plugin folder itself).

**CLI:**
```bat
xinsp-backend.exe --port=7823 --plugins-dir=C:\dev\my_plugins
```

**Env var:**
```bat
set XINSP2_EXTRA_PLUGIN_DIRS=C:\dev\my_plugins
```

The host scans the parent folder; any subfolder containing a
`plugin.json` is registered.

On load, the host checks your DLL's ABI version and then loads it
straight through — plugins are **trusted**, there's no certification
gate. (Write your own tests to gain confidence; see step 7.)

---

### 5. See it in VS Code

In the **Instances** view, click the `+` icon. You'll get a QuickPick
listing all registered plugins — pick `my_first_plugin`. Type an
instance name (e.g. `t0`).

Single-click the new tree row, or click its inline gear icon, to open
the plugin's webview. Drag the **Threshold** slider, toggle **Invert**,
click **Apply** — your C++ code runs.

---

### 6. Edit. Rebuild. Hot reload.

Change anything in `my_first_plugin.cpp`, then:

```bat
cmake --build build --config Release
```

The host detects the DLL change, unloads the old one, loads the new
one, **and restores instance state** (via `get_def`/`set_def`). No
restart, no project reopen.

---

### 7. Test it

### Native (C++) — fast, headless

```bat
cmake --build build --config Release --target my_first_plugin_test
.\my_first_plugin_test.exe
```

Runs your `XI_TEST(...)` blocks (a useful starter set: create/destroy,
`get_def`/`set_def` round-trip, `process()` on empty input).

### UI E2E (JavaScript) — drives the live webview

**Cold session** (clean state every time, CI-friendly):
```bat
node C:\dev\xInsp2\sdk\testing\run_ui_test.mjs .
```

**Warm session** (faster inner loop, in your already-open VS Code):
Cmd Palette → `xInsp2: Run Plugin UI Tests` → pick the folder.

Both load `tests/test_ui.cjs` and pass it the same `h` helpers object.
Screenshots land in `tests/screenshots/`.

---

## What to read next

| Context | File |
|---------|------|
| You want the complete API reference for THIS plugin | `my_first_plugin/README.md` (auto-generated, 500 lines) |
| You want the SDK's overview + cheatsheets | `xInsp2/sdk/README.md` |
| You want to see real working examples | `xInsp2/sdk/examples/` (hello, counter, invert, histogram, trigger_source) |
| You want a complex production-grade plugin | The `ct_shape_based_matching` plugin in the parent `xInsp/plugins/` tree (OpenCV + AVX2 + UI + per-instance template storage) — an out-of-tree plugin that consumes xInsp2 the same way yours will |
| You want the image-source / multi-camera path | `xInsp2/sdk/examples/trigger_source/` (a source using `emit_record`) + `xInsp2/examples/stereo_sync/` (a gathering plugin combining paired frames) |
| You want the C ABI definition | `xInsp2/backend/include/xi/xi_abi.h` |
| You want the test framework | `xInsp2/backend/include/xi/xi_test.hpp` |

---

## Common gotchas (read these once)

- **The DLL goes next to `plugin.json`, not into a `build/Release/`
  folder.** The cmake helper takes care of this — don't override
  `RUNTIME_OUTPUT_DIRECTORY_RELEASE` unless you want to break the
  scan.
- **`XINSP2_ROOT` is required at cmake-configure time.** Either set
  the env var or rely on the sibling-folder auto-detection. If you see
  "XINSP2_ROOT is not set" — that's the fix.
- **Plugin name uniqueness is global.** If two folders have the same
  `name` in `plugin.json`, the later-scanned one wins.
- **Hot-reload preserves instance state.** Cache mutable fields in
  your class, not in statics — statics get re-initialized on reload.
- **`folder_path()` is empty until a project is loaded.** If your
  plugin needs disk space and there's no project, fall back gracefully
  or just queue the write.

---

## Need help?

The scaffolded plugin's `README.md` has a complete API reference and a
copy of every common pattern as runnable code. Read it once, keep it
open while you work.
