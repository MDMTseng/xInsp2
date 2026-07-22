# 35 — core-sheds-OpenCV: the backend exe no longer links OpenCV

Status: **LANDING** on `exp/core-shed-opencv` (branched off `polaris2_main`,
2026-07). Completes OQ-9 for the backend: the plugins build already decoupled
OpenCV (`toolbox/CMakeLists.txt`: "OpenCV — NO LONGER a mandatory dependency"); the
backend's `xi_core` was the last target still forcing it onto everything.

## What OpenCV actually did in the core (nothing)

Audited every `cv::` site. The core RUNTIME (dispatch / lifecycle / pools / trigger /
emit / ws / service) calls **zero** OpenCV operations — `service_main` only holds an
`opencv_dir` STRING to hand the plugin/script compiler. The only real `cv::` code was:

1. **`xi_jpeg.hpp` `encode_jpeg_opencv`** — a JPEG fallback (imencode + cvtColor).
   REMOVED (see below).
2. **`xi_cv.hpp`** — the OPT-IN SDK bridge (`as_cv_mat/as_cv_read/as_cv_write`) that
   plugin/script authors include to do vision. KEPT — it is the SDK, not the core.

Everything else (`xi.hpp`, `xi_image.hpp`, `xi_jpeg.hpp` after the cut) is OpenCV-free.
The backend exe linked `opencv_world.dll` only because `xi_core` (an INTERFACE lib)
unconditionally did `target_link_libraries(xi_core INTERFACE ${OpenCV_LIBS})` —
every consumer inherited it, needed or not.

## Changes

- **Removed the OpenCV JPEG fallback** (`xi_jpeg.hpp`): `encode_jpeg_opencv` and the
  CPU-vendor probe (`detect_cpu_vendor`/`CpuVendor`, which existed only to pick
  OpenCV-vs-turbo) are gone. Dispatch is now **turbo → stb**, both OpenCV-free.
  `XINSP2_HAS_OPENCV` no longer gates anything in any header.
- **Decoupled `xi_core` from OpenCV** (`backend/CMakeLists.txt`): OpenCV is now a
  separate `xi_opencv` INTERFACE target (include dirs + libs). `xi_core` carries none.
  Only targets that include the opt-in cv:: SDK link `xi_opencv`:
  - `test_image_blob` (uses `as_cv_read`/`as_cv_write_blob`),
  - `script_selfcheck` / `template_selfcheck` / `examples_gate` (they MODEL user SDK
    code — force-include `xi_plugin_support.hpp`/`xi_script_support.hpp`, which pull
    `xi_cv.hpp`).
  Dropped the now-dead `XINSP2_HAS_OPENCV=1` from `xinsp_backend` and `bench_jpeg`.

## Verified (dumpbin /dependents)

```
xinsp-backend.exe : NO opencv/turbojpeg import   ← core is OpenCV-free
xinsp-runner.exe  : NO opencv/turbojpeg import
bench_jpeg.exe    : turbojpeg.dll only (no opencv)
test_image_blob.exe : opencv_world4100.dll        ← correct: it uses the cv:: bridge
```

Full backend + plugins ctest green.

## The boundary (what still touches OpenCV, on purpose)

- **Runtime-compiled user scripts/plugins** DO vision and DO use OpenCV. They get it
  from the **toolchain's own compile command** (`xi_script_compiler.hpp`: force-include
  `opencv2/opencv.hpp` + `/I <opencv_dir>/include`), which is independent of the
  `xi_core` CMake target — so this decoupling does not touch them. `opencv_world.dll`
  is STILL deployed next to the backend exe by `_xinsp_deploy_dlls`, but now for those
  runtime scripts (loaded into the process), NOT for the backend's own code.
- `find_package(OpenCV REQUIRED)` stays: the SDK-model tests + the toolchain path
  probe still want it at build time. Making the whole build OpenCV-optional (so a
  core-only build needs no OpenCV at all) is a possible follow-up, not done here.

## Net

The core binary is now free of both heavy image deps: **turbojpeg** lives only in the
imgcodec plugin (doc 34), and **OpenCV** is opt-in SDK for plugins/scripts. `xinsp-
backend.exe` links neither.
