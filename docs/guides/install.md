# Installing xInsp2 on a new machine (Windows)

This is the per-machine setup for **building and running** xInsp2 from source.
For the prebuilt end-user zip, see the [README "Install — end users"](../../README.md#install--end-users).

> **Why a compiler is required at runtime.** xInsp2's core is deliberately tiny;
> the power comes from composing plugins, and the per-project inspection *script*
> (and any project-local plugins) are compiled **on the fly** by the backend with
> `cl.exe`. So the MSVC toolchain + OpenCV must be present on any machine that
> opens/edits a project — not just on a build box. The only exception is a locked
> production line that ships **pre-compiled** script/plugin DLLs and never
> recompiles.

---

## 0. One-click (recommended)

From a checkout, run the setup script — it installs the required toolchain
(MSVC Build Tools + OpenCV) into the locations the backend probes, and finishes
with a health check. Re-runnable; skips anything already present.

```powershell
# elevated PowerShell (VS Build Tools install needs admin)
powershell -ExecutionPolicy Bypass -File tools\setup-windows.ps1 -IncludeOptional
```

`-IncludeOptional` also installs libjpeg-turbo. Intel IPP has no winget package —
install it manually (it's auto-detected under `C:\Intel\ipp\<ver>`). Other flags:
`-SkipVS`, `-SkipOpenCV`, `-OpenCvVersion <x.y.z>`.

Prefer to do it by hand (or understand what it installs)? The manual steps follow.

---

## 1. Required (two things — nothing works without these)

### ① MSVC C++ toolchain — Visual Studio Build Tools 2022
Gives `cl.exe`, `vcvars64.bat`, the Windows SDK, and C++20.

```powershell
winget install Microsoft.VisualStudio.2022.BuildTools
```

During install **tick the "Desktop development with C++" workload** — without it
you get the shell but no compiler. Full Visual Studio 2022 Community works too.
VS 2019 is also fine (CMake/OpenCV accept its `vc16` toolset).

The backend auto-finds `vcvars64.bat` under the standard VS install roots
(`xi_script_compiler.hpp::auto_find_vcvars`). If yours is non-standard, pin it
per-project (see [§5](#5-verify--fix-paths-in-the-editor)).

### ② OpenCV 4.x
Mandatory: `find_package(OpenCV REQUIRED)`, and `xi.hpp` / `xi_plugin_support.hpp`
force-include `<opencv2/opencv.hpp>`, so every script and plugin needs it.

1. Download the Windows build from [opencv.org](https://opencv.org/releases/) and
   self-extract to `C:\opencv` → you get `C:\opencv\opencv\build` (the probe's
   default location — put it here and no config is needed).
2. Confirm `C:\opencv\opencv\build\x64\vc16\lib\OpenCVConfig.cmake` exists
   (prebuilt OpenCV ships a `vc16` toolset; `vc17` also works).
3. If you install it elsewhere, set the `OpenCV_DIR` environment variable to that
   `lib` folder, **or** pin it per-project in the editor (§5).

> Runtime DLLs (`opencv_world*.dll`) are auto-copied next to the built exes by
> CMake's `_xinsp_deploy_dlls`, so running from `backend/build/Release` needs no
> PATH changes.

---

## 2. To build from source

```powershell
winget install Kitware.CMake      # CMake >= 3.16
winget install Git.Git            # to clone the repo
```

```powershell
cmake -S backend -B backend/build -A x64
cmake --build backend/build --config Release
cmake -S plugins -B plugins/build -A x64
cmake --build plugins/build --config Release
```

To opt the optional accelerators into the build, add `-DXINSP2_HAS_TURBOJPEG=ON`
and/or `-DXINSP2_HAS_IPP=ON` to the backend configure step.

---

## 3. For the VS Code dev experience (optional; headless production doesn't need it)

```powershell
winget install Microsoft.VisualStudioCode
winget install OpenJS.NodeJS      # 18+, to build the extension
```

```powershell
cd vscode-extension
npm install
npm run build
```

Also install the **Microsoft C/C++ extension** (`ms-vscode.cpptools`) — opening a
project writes a `.vscode/c_cpp_properties.json` that this extension reads, giving
you go-to-definition and squiggle-free `<xi/...>` / OpenCV resolution. (xInsp2
auto-recommends it via a generated `.vscode/extensions.json`.) See
[`writing-a-script.md`](writing-a-script.md).

---

## 4. Optional accelerators (skip and the framework falls back)

| Library | Install | Probe default | Fallback if absent |
|---|---|---|---|
| **libjpeg-turbo** | `winget install libjpeg-turbo.libjpeg-turbo.VC` | `C:\libjpeg-turbo64` | stb JPEG encode |
| **Intel IPP** | from Intel | `C:\Intel\ipp\<ver>` (auto-scanned) | OpenCV |

Both are **off by default** (`XINSP2_HAS_*`). Override their location with the
`TURBOJPEG_ROOT` / `IPP_ROOT` env vars or per-project (§5).

Python 3 is only needed to run the `driver.py` test harnesses / the SDK — not to
run the framework itself.

---

## 5. Verify & fix paths in the editor

Open the project in VS Code, then **Project Settings → C++ Toolchain**. It shows
each component's resolved path, where it came from (override / environment /
built-in default), and whether it exists:

- **xi headers · OpenCV · MSVC** — required; missing → red warning.
- **libjpeg-turbo · IPP** — optional; missing → grey "not installed".

For any wrong/missing row, click **Set path…** to pick the correct folder (or
`vcvars64.bat` for MSVC). The path is saved as a per-project override in
`project.json`:

```json
{
  "toolchain": {
    "opencv_dir": "D:/libs/opencv/build",
    "vcvars": "D:/VS/VC/Auxiliary/Build/vcvars64.bat"
  }
}
```

Resolution priority is **override → environment variable → built-in probe**, and
the same resolved paths feed both the compiler and IntelliSense, so they can't
drift. Recompile to apply (the panel offers a "Recompile now" button). See the
`toolchain_health` / `set_toolchain_override` commands in
[`../protocol.md`](../protocol.md).

---

<!-- TODO(linux): this guide is Windows-only. The Linux build path is deferred
     (see docs/design/linux-port.md); when it lands, document the gcc/clang +
     pkg-config OpenCV setup and the linux-clang-x64 IntelliSense variant. -->
