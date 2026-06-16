# Build & run

Set up a Windows machine, build xInsp2 from source, and run a project on day one.
(For the prebuilt end-user zip see the repo README.)

> **Why a compiler is required at runtime.** The core is deliberately tiny; the
> power is in composing plugins, and the per-project inspection *script* (and any
> project-local plugins) are compiled **on the fly** by the backend with `cl.exe`.
> So MSVC + OpenCV must be present on any machine that opens/edits a project — the
> only exception is a locked production line shipping **pre-compiled** DLLs (the
> AOT bundle, see [`deploy.md`](./deploy.md)).

## 1. One-click setup (recommended)

```powershell
# elevated PowerShell (VS Build Tools install needs admin)
powershell -ExecutionPolicy Bypass -File tools\setup-windows.ps1 -IncludeOptional
```

Installs the required toolchain (MSVC Build Tools + OpenCV) into the locations the
backend probes, then runs a health check. Re-runnable; skips what's present.
`-IncludeOptional` adds libjpeg-turbo. Flags: `-SkipVS`, `-SkipOpenCV`,
`-OpenCvVersion`. Prefer to do it by hand? The manual steps follow.

## 2. Required (nothing works without these)

- **MSVC C++ toolchain — VS Build Tools 2022** (`winget install
  Microsoft.VisualStudio.2022.BuildTools`). **Tick "Desktop development with C++"**
  or you get no compiler. VS 2019 (`vc16`) also works. The backend auto-finds
  `vcvars64.bat` under standard VS roots; pin a non-standard one per-project (§5).
- **OpenCV 4.x** — `find_package(OpenCV REQUIRED)` + `xi.hpp` force-includes it.
  Self-extract to `C:\opencv` → `C:\opencv\opencv\build` (the probe default; no
  config needed). Elsewhere → set `OpenCV_DIR` to the `lib` folder, or pin
  per-project. Runtime `opencv_world*.dll` are auto-copied next to the built exes.

## 3. Build from source

```powershell
winget install Kitware.CMake Git.Git
cmake -S backend -B backend/build -A x64
cmake --build backend/build --config Release      # → xinsp-backend.exe, xinsp-fe.exe, tests, DLLs
cmake -S plugins -B plugins/build -A x64
cmake --build plugins/build --config Release
```

Optional accelerators: add `-DXINSP2_HAS_TURBOJPEG=ON` / `-DXINSP2_HAS_IPP=ON` to
the backend configure.

## 4. VS Code dev experience (optional; headless production doesn't need it)

```powershell
winget install Microsoft.VisualStudioCode OpenJS.NodeJS   # Node 18+
cd vscode-extension && npm install && npm run build
```

Also install the **Microsoft C/C++ extension** (`ms-vscode.cpptools`) — opening a
project writes `.vscode/c_cpp_properties.json` for go-to-definition + squiggle-free
`<xi/...>` / OpenCV resolution (auto-recommended via `.vscode/extensions.json`).

## 5. Toolchain health check

**Project Settings → C++ Toolchain** shows each component's resolved path, its
source (override / env / built-in), and whether it exists. For a wrong/missing
row, **Set path…** saves a per-project override in `project.json`:

```json
{ "toolchain": { "opencv_dir": "D:/libs/opencv/build",
                 "vcvars": "D:/VS/VC/Auxiliary/Build/vcvars64.bat" } }
```

Priority is **override → env var → built-in probe**; the same resolved paths feed
both the compiler and IntelliSense, so they can't drift. Recompile to apply.

## 6. Run a project

**Optional accelerators** (off by default, framework falls back): libjpeg-turbo
(`C:\libjpeg-turbo64`, → stb), Intel IPP (`C:\Intel\ipp\<ver>`, → OpenCV).
Override with `TURBOJPEG_ROOT` / `IPP_ROOT`.

- **VS Code (fast loop):** open the repo with the xInsp2 extension, open a project
  under `examples/`, hit compile/run — the extension spawns the BE, compiles the
  script, streams vars to the viewer.
- **Headless (what FE/production does):**
  ```powershell
  backend/build/Release/xinsp-backend.exe --project=examples/qa_group_parallelism --autostart-fps=-1
  ```
  (`-1` = trigger-only; the project's sources drive it.)
- **HMI:** `node hmi/serve.mjs` then open the printed URL (or
  `hmi/index.html?ws=ws://127.0.0.1:7823/`).
- **First change:** edit a project's `inspect.cpp` and re-run — the backend
  hot-reloads the DLL. Try a `VAR(...)` / `xi::result(...)` in
  `examples/qa_run_result/inspect.cpp`.
- **Tests:** `python tools/run_qa.py` (all `examples/qa_*/driver.py`); `ctest` for
  the C++ unit tests. See `testing.md`.

---

<!-- TODO(linux): Windows-only. The Linux build path is deferred — see
     roadmap/linux-port.md. -->

## See also

- [`write-a-script.md`](./write-a-script.md) / [`write-a-plugin.md`](./write-a-plugin.md) — author the project.
- [`deploy.md`](./deploy.md) — ship to a target with no toolchain.
- [`../reference/ws-protocol.md`](../reference/ws-protocol.md) — `toolchain_health` / `set_toolchain_override`.
