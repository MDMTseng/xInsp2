# IPC + SHM (cross-process isolation) — REMOVED 2026-05

> **This component no longer exists.** The SHM mesh, worker process
> (`xinsp-worker.exe`), script-runner process (`xinsp-script-runner.exe`),
> `xi_shm.hpp`, `xi_ipc.hpp`, `worker_main.cpp`, and `script_runner_main.cpp`
> were all deleted in 2026-05 and are absent from the current codebase.

## What replaced it

FE and BE run as two separate **binaries** but share **one address space** for
the backend: the `xinsp-backend.exe` process is a single in-process compute
core supervised by `xinsp-fe.exe`. All plugins and the inspection script
execute directly in the backend's address space — no IPC pipe, no SHM region,
no `process:` isolation option.

Zero-copy image passing now means zero-copy **within the single backend
process** via the in-process `xi::ImagePool` (handles are pool-slot indices;
passing a handle is a `uint64_t` copy, not a memcpy). The five
`shm_create_image` / `shm_alloc_buffer` / `shm_addref` / `shm_release` /
`shm_is_shm_handle` fields remain declared in `xi_abi.h` for binary
compatibility but are hard-wired `nullptr` in `make_host_api()` — plugins must
null-check and fall back to `image_create` (which is what they do). The
`"isolation"` field in `instance.json` is accepted but ignored with a one-time
deprecation warning.

Crash containment is provided by per-thread minidumps, crash breadcrumbs, and
PDB symbolication instead of process isolation — see
[`guides/debugging.md`](../guides/debugging.md).

For the current in-process architecture see
[`docs/design/fe-be-split.md`](../design/fe-be-split.md).
