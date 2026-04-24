# Getting started

Build your first xInsp2 plugin from zero in about 5 minutes.

---

## Prerequisites

- A cloned xInsp2 repo (you're inside it now). Backend must be built:
  the cert step is what makes loading a plugin "safe by default" and it
  needs `xinsp-backend.exe`.
- CMake ≥ 3.16
- A C++ compiler (MSVC 2019+ on Windows, gcc/clang on Linux)
- Node.js (only needed for the scaffold tool and UI tests)
- Optional: VS Code with the xInsp2 extension built/loaded

That's it — no npm dependencies for the plugin itself.

---

## 1. Pick a folder for your plugins

It can be anywhere — your home dir, a sibling of the xInsp2 checkout,
inside a different repo. Plugins are *external* by design.

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

## 2. Scaffold a new plugin

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
│   ├── test_native.cpp    ← baseline + 2 starter XI_TEST blocks
│   └── test_ui.cjs        ← UI E2E test (drives the live webview)
└── README.md              ← 500-line offline kit, plugin-specific
```

The `README.md` inside the new folder is the **complete reference** —
API surface, Record/Json/Image cheatsheets, lifecycle diagram, UI
protocol diagram, common patterns, pitfalls. Open it.

---

## 3. Build it

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

## 4. Load it into xInsp2

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

On first load, the host runs an 8-test **baseline** against your DLL
(C-ABI safety: doesn't crash on empty input, no leaks, JSON round-trips,
etc). On pass it writes `cert.json` next to the DLL — subsequent loads
of the same DLL skip the baseline. On fail it logs the failing tests
and refuses to instantiate.

---

## 5. See it in VS Code

In the **Instances** view, click the `+` icon. You'll get a QuickPick
listing all registered plugins — pick `my_first_plugin`. Type an
instance name (e.g. `t0`).

Single-click the new tree row, or click its inline gear icon, to open
the plugin's webview. Drag the **Threshold** slider, toggle **Invert**,
click **Apply** — your C++ code runs.

---

## 6. Edit. Rebuild. Hot reload.

Change anything in `my_first_plugin.cpp`, then:

```bat
cmake --build build --config Release
```

The host detects the DLL change, unloads the old one, loads the new
one, **and restores instance state** (via `get_def`/`set_def`). No
restart, no project reopen, no rerunning the baseline.

---

## 7. Test it

### Native (C++) — fast, headless

```bat
cmake --build build --config Release --target my_first_plugin_test
.\my_first_plugin_test.exe
```

Runs the same 8 baseline tests + your custom `XI_TEST(...)` blocks.
A passing run writes `cert.json` proactively — the host won't re-run
the baseline next time.

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
| You want to see real working examples | `xInsp2/sdk/examples/` (hello, counter, invert, histogram) |
| You want a complex production-grade plugin | `<plugins>/ct_shape_based_matching/` (OpenCV + AVX2 + UI + per-instance template storage) |
| You want the C ABI definition | `xInsp2/backend/include/xi/xi_abi.h` |
| You want the test-framework + baseline source | `xInsp2/backend/include/xi/xi_test.hpp`, `xi_baseline.hpp` |

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
- **Cert is invalidated by any DLL change** (size or mtime). After
  rebuild, the host re-runs the baseline on next load. Run the test
  binary to write cert proactively.
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
