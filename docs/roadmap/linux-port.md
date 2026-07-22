# Cross-platform port (Linux / ARM / macOS) — inventory, effort + going-forward rule

> **Status: Linux (aarch64) build EXECUTED on the `linux-build` branch.** The
> headless backend now configures, builds, links, and passes the ctest suite on
> Linux/GCC (developed on ARM64 — Raspberry Pi, Debian, g++ 14). Phase 0 (build
> green), Phase-1 core loop (runtime g++ compile driver + `.so` load), Phase-2 FE
> supervisor, and the Phase-3 crash-forensics **stop-gap** are done and validated
> end-to-end. The Phase-4 watchdog needed NO port work — the design already
> evolved off `TerminateThread` to cooperative-cancel + `std::_Exit` (portable);
> it is validated on Linux via `qa/qa_watchdog` (real g++-JIT'd runaway
> script → cooperative cancel → HARD trip → exit). Real **Breakpad** minidumps
> are now wired as an opt-in (`-DXINSP2_HAS_BREAKPAD=ON`); the default build keeps
> the text-backtrace sidecar. The headless Linux backend is functionally complete.
> What landed:
>
> - **Breakpad minidumps (opt-in):** `-DXINSP2_HAS_BREAKPAD=ON -DBREAKPAD_ROOT=<checkout>`
>   links Google Breakpad's client lib into **only** the backend + runner (via
>   `_xinsp_add_breakpad`); the single dependent TU is `backend/src/crash_breakpad.cpp`,
>   so the rest of the tree + all tests stay Breakpad-free. It registers a
>   minidump-writer hook that `xi_crash_dump.hpp` calls from the terminate/SIGABRT
>   path — producing a REAL `.dmp` (validated: `file` → "Mini DuMP crash report,
>   13 streams") next to the FE-parseable `.json` sidecar. Off by default → the
>   text-backtrace sidecar. Because the SEH translator owns the fatal signals (so a
>   plugin fault is catchable), the dump is taken on the UNCAUGHT path (terminate),
>   so the faulting thread's stack is unwound to the terminate frame; precise
>   fault-site module blame is preserved separately via `xi::last_fault_addr()`.
>   Build Breakpad: `git clone https://chromium.googlesource.com/breakpad/breakpad`
>   + its `linux-syscall-support` into `src/third_party/lss`, `./configure && make
>   src/client/linux/libbreakpad_client.a`.
> What landed (earlier):
>
> - **Watchdog (`service_main.cpp`):** already portable — `std::thread` monitor,
>   atomic per-worker deadlines, Phase-1 cooperative cancel via the script's
>   `set_global_cancel` thunk, Phase-2 hard trip = `std::_Exit(WATCHDOG_EXIT_CODE)`
>   for FE respawn. No `TerminateThread`. `qa/qa_watchdog/driver.py` is now
>   cross-platform (was `nt`-only because the JIT compile was) and PASSES on Linux.
>   (Note: `std::_Exit(0x5744)` truncates to `0x44` on POSIX — exit codes are 8-bit;
>   the FE records it either way, and does not branch on the value.)
> - **JIT fix:** the g++ driver compiles `vendor/yyjson/yyjson.c` in its OWN C
>   invocation (a cached `.o`) rather than inline — the C++-only force-includes
>   (`-include opencv2/opencv.hpp`) apply to every input and broke it as a C file.
>
> - **FE supervisor (`fe_main.cpp`):** POSIX `run_supervisor` — fork/exec the
>   backend into its own process group, `waitpid`(WNOHANG) monitor, boot-readiness
>   gate + heartbeat + health-mirror + respawn/quarantine policy (all the shared
>   portable logic), `killpg` shutdown, SIGINT/SIGTERM handlers, BSD-socket port
>   probe. Validated: spawn→healthy→SIGKILL-a-child→crash-history record→respawn→
>   clean shutdown with no orphans. GAP vs the Job Object: an FE that is itself
>   SIGKILLed orphans the backend (reparented to init) — cgroup/pidfd is the
>   follow-up; clean + signalled FE exits kill the backend.
> - **Crash forensics stop-gap (`xi_crash_dump.hpp`):** POSIX `install()` hooks
>   `std::set_terminate` (the fault→seh_exception→uncaught path) + `SIGABRT`, and
>   writes the SAME `.json` crash-report the FE parser reads (exception.name from
>   the seh_exception, module blame via `dladdr(xi::last_fault_addr())`, the
>   portable breadcrumb `context`/`culprit`/`threads`) beside a `.dmp` backtrace,
>   printing the `minidump:` trigger line. Composes with the SEH translator instead
>   of fighting it for SIGSEGV. Validated: backend crash → sidecar → FE enriches
>   its crash-history (`exception`, `report`, `dump`). Precise plugin-quarantine
>   attribution still wants a real dump (follow-up).
>
> Earlier legs (Phase 0/1):
>
> - **CMake:** configures with the system OpenCV (`find_package`), Ninja + g++.
>   Tree-wide GCC flags added: `-fno-gnu-unique` (modules truly unload on
>   `dlclose` → hot-reload works + per-load module statics), `-fnon-call-exceptions`
>   (the fault→exception unwind, below).
> - **`xi_dynlib.hpp` (new):** a drop-in for the loader's `<windows.h>` — on POSIX
>   it maps `HMODULE`/`LoadLibraryExA`/`GetProcAddress`/`FreeLibrary`/
>   `GetModuleHandleExA`/`GetModuleFileNameA`/`GetCurrentProcessId` + the
>   `LOAD_LIBRARY_SEARCH_*` flags onto `dlopen`/`dlsym`/`dlclose`, so the loader
>   family compiles unchanged. `AddDllDirectory`/`RemoveDllDirectory` are inert.
> - **`XI_EXPORT`:** `__declspec(dllexport)` → `__attribute__((visibility("default")))`,
>   defined once in `xi_abi.h`; plugin/script export macros + raw test fixtures use it.
> - **SEH (`xi_seh.hpp`):** `install_seh_translator()` now installs a
>   `sigaction(SIGSEGV/SIGFPE/SIGBUS/SIGILL)` handler that THROWS `seh_exception`,
>   so the existing `try/catch(seh_exception)` sites catch hardware faults exactly
>   as on Windows. Needs `-fnon-call-exceptions`; uses a per-thread `sigaltstack`
>   for stack-overflow faults, classified via the thread's recorded stack bounds.
> - **Compile driver (`xi_script_compiler.hpp`):** a POSIX `compile_posix_build_`
>   spawns `g++ -fPIC -shared -fnon-call-exceptions -fno-gnu-unique -fpermissive`,
>   OpenCV via `pkg-config`, yyjson compiled straight from its vendored `.c`,
>   `.so` output; plus a gcc/clang diagnostic parser (`parse_diagnostics_gcc`).
> - **Certify (`xi_certify.hpp`):** `run_certify_subprocess` ported to
>   `fork`+`execl`+`waitpid` (timeout → SIGKILL); `certify_in_process` + the
>   scan-time gate now run on POSIX via the dlopen shim.
> - Already-portable leaves confirmed working: `xi_ws_server` (BSD sockets),
>   `xi_atomic_io`, `xi_owner_lock`, `xi_cli_args`.
>
> The original parking-lot inventory below is kept for the remaining legs
> (crash dumps, FE supervisor, watchdog redesign, macOS).
>
> **Historical note (pre-port):** This was a parking lot for "things that
> need to change when we eventually port off Windows"; the actual port
> is now underway on `linux-build`.
>
> **TL;DR.** Moderate, not scary. Only a minority of backend files touch Win32
> (roughly a third), and most of those are already `#ifdef _WIN32`-gated; the OS
> coupling is concentrated, not smeared. A working **Linux x64**
> headless backend (build + load + clang compile + core trigger loop) is
> ~2–3 weeks; **ARM Linux** adds ~2–4 days; **macOS** adds ~1–2 weeks. The
> only genuinely hard items are crash-forensics parity and the watchdog
> redesign. See "Effort + phasing", "ARM & macOS deltas", and "The
> runtime-compile constraint" sections below for the full picture.

---

## Deferred on the Linux port (with reasons)

The headless backend is functionally complete + ctest-green on Linux/aarch64.
These are known, deliberately-deferred items (not blockers), kept here so the
next round has a record:

- **`tools/gate.py` doesn't run on Linux yet.** Its `build` stage assumes the
  Windows `build/Release` layout, and the `docs`/`sdk` stages need `node` on
  PATH (absent on the Pi). The Linux batches are validated with `ninja` +
  the full `ctest` (which also runs the doc-coverage / retired-terms gates) +
  the relevant `qa/qa_*` e2e driver. Making `gate.py` platform-aware
  (build-linux layout, skip/soft-fail node stages when node is absent) is the
  port item so the ONE authoritative gate covers Linux too.
- **FE↔backend Job-Object orphan parity.** `killpg` covers clean + signalled FE
  exits; an FE that is itself SIGKILLed orphans the backend. Needs a cgroup or
  pidfd babysitter for full parity (`fe_main.cpp`).
- **`build:cmake` plugin build is not timeout-bounded on POSIX.** The Windows
  path kills the tree after `kCmakeBuildTimeoutMs`; the POSIX `popen` path is
  functional but unbounded (a wedged toolchain stalls the control plane). A
  fork/exec + poll-timeout + `killpg` version (cf. certify's fork+waitpid) is
  the follow-up (`xi_cmake_build.hpp`).
- **Per-thread dispatch affinity/scheduling** (`service_dispatch.cpp`) stays a
  no-op. Process-level priority (`setpriority`) is done; thread affinity
  (`pthread_setaffinity_np` / `cpu_set_t`) + per-worker scheduling are not.
- **`qa/qa_*/driver.py` e2e drivers — 39 ported + passing on Linux.**
  Recipe: `ports.backend_exe()`/`fe_exe()` + drop the `os.name != "nt"` skip +
  `LOCALAPPDATA/Temp` → `tempfile.gettempdir()` (env `TMPDIR`); Windows process
  tooling (`taskkill`/`tasklist`/PowerShell) → POSIX (`os.kill`/`/proc/<pid>/fd`).
  Covers three sets: the **script-only** set (qa_watchdog, qa_edge, qa_run_result,
  qa_min_interval, qa_group_*, …), the **pack-*/multi-plugin** set (qa_pack_* ×9,
  qa_multi_graph, qa_kv_reload, qa_use_pack_door, qa_resource_handle,
  qa_remove_under_load, qa_corrupt_project_json — all loading REAL global plugins),
  and the **imgcodec** set (qa_cap_imgcodec, qa_cap_imgcodec_autoload,
  qa_jpeg_preview — `xi.jpeg.encode`/`xi.image.decode`, verified with the default
  stb encoder; turbojpeg optional for 3.2×). The global-plugin-loading and imgcodec
  gaps that used to block ~20 of these are now CLOSED (see the plugin-loading work
  above). A handful stay nt-only for specific remaining gaps (each recorded, so
  they SKIP not FAIL):
    - qa_cpu_affinity — per-thread CPU affinity is a no-op on Linux (deferred item above).
    - qa_local_auto — the local auto image source delivers no frames on Linux.
    - qa_export_bundle — `tools/export_bundle.py` is Windows-only (builds an MSVC `.dll` bundle).
    - qa_lifecycle_teardown — opens fixture projects (`burst_pipeline`, `qa_sink_shared_doc`) absent from this branch.
    - qa_slow_consumer — the WS wedged-client-drop timing gap (deferred item below).
  The two **FE-based** drivers (qa_func, qa_recover) now pass. qa_func FE-E9
  surfaced a REAL port bug: `fe_main.cpp discover_sibling_exe()` located the
  sibling backend via `fs::current_path()` (cwd), so an FE launched from a
  foreign cwd (fe.json scratch dir) `execv`-failed and never spawned the backend
  — fixed to read `/proc/self/exe` (commit *resolve sibling backend from FE exe
  dir*). Both need a longer `--boot-timeout-ms` because each backend autostart
  (and each recover respawn) cold-compiles the project plugin + script on this
  slow ARM box, well over the FE's default 60 s boot window.
- **Precise plugin-quarantine crash attribution** wants a real dump on the
  faulting thread; the terminate-path minidump unwinds the fault frame (module
  blame is preserved via `xi::last_fault_addr()`, but the culprit cross-check is
  weaker). A signal-time dump would conflict with the fault-catching model.
- **WS wedged-client-drop detection window is too wide on Linux** (qa_slow_consumer,
  the only pack-*/multi-plugin driver still nt-only). The other 15 pack-* drivers
  pass — global plugins load fine — but qa_slow_consumer's phase 2 fails: a wedged
  client (slow/0-drain reader) is NOT dropped within its 12 s budget, so the
  single-client slot never frees. `xi_ws_server.hpp` drops a wedged peer via the
  `kOutboundHardCapBytes` outbound byte budget + `SO_SNDTIMEO`; but as the header's
  own comment notes, a **large SNDBUF widens the wedge-detection window** (the
  kernel silently absorbs the backlog before `::send` blocks), and the 2026-07
  merge added **adaptive per-connection SNDBUF** — so on Linux a slow consumer's
  modest backlog is absorbed and the byte cap isn't hit inside 12 s.
  `test_ws_async_writer` (unit) passes because it floods straight to the byte cap;
  only the e2e slow-consumer shape exposes it. Fix: cap the adaptive SNDBUF for
  wedge-detection purposes, or lower `kOutboundHardCapBytes` on Linux, so a wedged
  peer trips the drop within the budget.

## Going-forward rule

**Any new code or bug fix from this point on should be cross-platform
unless there's a hard reason it can't be.** That means:

- New Win32 API call site → must have a portable abstraction (or at
  minimum `#ifdef _WIN32` with a TODO for the Linux side).
- New file in `backend/include/xi/` should compile on Linux out of the
  box if it doesn't touch OS primitives. If it does, isolate them
  behind a small header (like `xi_atomic_io.hpp` already does for
  Windows-only via `#ifdef`). **A new FE-supervisor-only header goes in
  `fe/include/xi/` (linked via the `xi_fe` CMake target), not
  `backend/include/xi/`** — see the include-tree section below.
- Don't assume `cl.exe`, MSVC paths, `_strdup`, `MAX_PATH`,
  backslashes, CP-950, etc.
- Tests added should not require Windows tools (`PowerShell`,
  `cmd /C`, `taskkill`).

When unavoidable Windows-only code IS added, mark it with a
`// TODO(linux):` comment naming the Linux equivalent so the eventual
porter has a starting point.

---

## What's currently Windows-only

Categorised by porting effort. None of this needs to be done before
the port itself; it's a record so we know what to expect.

### Easy — drop-in API replacement (~half day each)

| File | Win32 mechanism | Linux equivalent |
|---|---|---|
| `backend/include/xi/xi_ws_server.hpp` | `winsock2` + `WSAStartup` + `closesocket` | BSD `<sys/socket.h>`; existing `select()` is already POSIX |
| `backend/include/xi/xi_atomic_io.hpp` | `CreateFileW` + `MoveFileExW` + `FlushFileBuffers` | `open()` + `write()` + `fsync(fd)` + `fsync(dirfd)` + `rename()` |
| `backend/include/xi/xi_script_loader.hpp` / `xi_plugin_manager.hpp` / `xi_cabi_adapter.hpp` | `LoadLibraryA`/`LoadLibraryExA` / `GetProcAddress` / `FreeLibrary` / `GetModuleFileNameA` (the `GetProcAddress` symbol resolution + `HMODULE` handle live in `xi_cabi_adapter.hpp` since the manager split) | `dlopen` / `dlsym` / `dlclose` / `dladdr` |
| `backend/include/xi/xi_cli_args.hpp` `get_exe_dir()` | `GetModuleFileNameA` (NOW has an `#else` `read_symlink("/proc/self/exe")` branch) | `/proc/self/exe` (Linux) / `_NSGetExecutablePath` (macOS) |
| `_strdup` / `_dupenv_s` / `_set_se_translator` | MSVC | `strdup` / `getenv` / signal handler |
| `MAX_PATH`, `WCHAR`, `wstring` paths | Win32 | `std::filesystem` already portable; drop wide-char conversions |
| `service_main.cpp` `set_os_thread_priority_` / `set_os_thread_affinity_` (dispatch-group worker tuning) | `SetThreadPriority` / `SetThreadAffinityMask` + `GetProcessAffinityMask` / `GetCurrentProcessorNumber` | `pthread_setschedprio`/`nice`; `pthread_setaffinity_np` + `cpu_set_t` / `sched_getcpu`. Both already `#ifdef _WIN32` + `TODO(linux)`. |
| `service_main.cpp` `main()` perf knobs (timer res + `--priority`) | `winmm timeBeginPeriod(1)` (runtime-loaded); `SetPriorityClass` | Linux `nanosleep`/`clock_nanosleep` already high-res (no-op); `setpriority(PRIO_PROCESS)` / `sched_setscheduler`. Already `#ifdef _WIN32` + `TODO(linux)`. |

### Medium — rewrite a module (1-3 days each)

| File | Win32-only mechanism | Linux equivalent |
|---|---|---|
| `backend/include/xi/xi_seh.hpp` | `_set_se_translator` + `__try`/`__except` | `sigaction(SIGSEGV / SIGFPE / SIGBUS)` + `sigsetjmp`/`siglongjmp`. Or wire Google Breakpad. |
| `backend/include/xi/xi_script_compiler.hpp` | `cl.exe` + `vcvars64.bat` + `cmd /C` | `g++` or `clang++` direct spawn; rewrite the diagnostic parser for gcc / clang error format. |
| Crash forensics — `backend/include/xi/xi_crash_dump.hpp` (`xi::crash::`, extracted 2026-06 from `service_main.cpp`) | `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` + `EnumProcessModules` + `AddVectoredExceptionHandler` + the CRT death-path interceptors. All `#ifdef _WIN32`-gated; on non-Win `install()`/`reserve_fault_stack()` are no-ops and the breadcrumb model (`Context`/`ctx()`/`set()`) stays portable. | Google Breakpad or manual `sigaction` + core-file generation; `dl_iterate_phdr` for module-blame addresses. Replace the `#else` stubs in the leaf. |
| `backend/include/xi/xi_cmake_build.hpp` (`xi::cmake_build::`, host-side cmake invocation for build:cmake plugins, extracted 2026-06) | `_popen`/`_pclose` via `cmd.exe` in `run_cmd_capture` | `popen(cmd + " 2>&1")` — same shape, no outer-quote wrap. Already `#ifdef _WIN32` + `TODO(linux)` stub. |
| `backend/include/xi/xi_owner_lock.hpp` (`xi::ownerlock::`, F5 advisory single-writer stamp on a project folder, added 2026-06) | process-liveness probe: `GetCurrentProcessId` + `OpenProcess(SYNCHRONIZE)` + `WaitForSingleObject` | `getpid()` + `kill(pid, 0)` (EPERM = alive-but-other-user). Already `#ifdef _WIN32` + `TODO(linux)` `#else` branch; the stamp read/write is pure `std::filesystem`/`fstream`. |
| `backend/src/fe_main.cpp` (the `xinsp-fe` supervisor) | `CreateProcessA` + `WaitForSingleObject`, Job Object (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), `SetConsoleCtrlHandler`, Winsock TCP probe. | `posix_spawn`/`fork`+`execv`; `waitpid`/`pidfd`; `prctl(PR_SET_PDEATHSIG)` for the kill-on-parent-death guarantee; `sigaction(SIGINT/SIGTERM)`; POSIX `connect` probe. The whole Win32 path is already gated `#ifdef _WIN32` with a `TODO(linux)` stub `main()`; the crash-log parsing is portable. (PLC line-safe is **not** in the supervisor — it's a comms plugin's own sidecar process, see [`../internals/comms-sidecar.md`](../internals/comms-sidecar.md).) |

### Hard — different semantics (re-design)

| Mechanism | Why hard |
|---|---|
| ~~`TerminateThread` watchdog~~ **(RESOLVED — ports as-is; validated on Linux)** | **Update 2026-07-11 (`93de38b`): the cooperative-cancel layer was retired.** The watchdog no longer kills threads or soft-cancels — it is one phase: overrun → grace → hard `std::_Exit(WATCHDOG_EXIT_CODE)` + FE respawn. `_Exit` is POSIX, so the mechanism ports as-is; the only Linux work is the supervisor-respawn side (`fe_main.cpp` row above, now done). Validated on Linux via `qa/qa_watchdog` (a g++-JIT'd runaway script hard-trips and the FE respawns). The old sketches (cooperative-checkpoint API, `SIGUSR`+longjmp) are obsolete. |
| Plugin DLL versioning (`stem_vN.dll` per `xi_script_compiler.hpp`) | Exists only because Windows holds a load lock on the previous DLL until unload completes. Linux `dlclose` has no such lock, so this whole hack can be deleted on the Linux build. Make sure to keep it conditional, not retroactively rip it out from Windows. |

## Outside the backend

| Component | Windows-only piece | Linux replacement |
|---|---|---|
| `vscode-extension/test/e2e/*.cjs` | PowerShell + `System.Drawing.CopyFromScreen` for screenshots | `scrot` / `xdotool` / `grim`; the rest of the e2e is pure TS and portable |
| `tools/xinsp2_py/xinsp2/screenshot.py` | Same PowerShell path | Same alternatives |
| `sdk/templates/*/CMakeLists.txt.standalone` + scaffold | MSVC-specific flags (`/MD /EHa /utf-8`, `cl.exe`-style includes) | Already mostly portable; tighten any `if (MSVC)` blocks; ship matching gcc/clang flags |
| `vscode-extension/test/runE2E.mjs` etc that call `taskkill` | Win-only process kill | `pkill` / Node `process.kill()` |

## Recent additions audited as cross-platform-clean

| Addition | Notes |
|---|---|
| `qa/multi_source_surge/` (FL r6) | Pure `<thread>` + `<chrono>` + `<atomic>` + the `xi_*` portable headers. No Win32 calls in plugins or inspect. Builds via the same `cl.exe` path the rest of the SDK uses; on Linux it'll go through whatever the script compiler abstracts to. |
| `dispatch_stats` watchdog warning log on `cmd:start` (FL r6) | Pure C++; lives in service_main.cpp's existing log-emission path which is already non-Win-specific. |
| `examples/cross_proc_trigger/` (Task #74 validation) | C++ plugin source + Python driver use only `<chrono>`, `<cstdint>`, `<atomic>`, `<thread>`, the portable `xi_*` headers, and Python stdlib. No Win32. (Note: the cross-process path this validated was removed 2026-05; the example no longer exercises a live feature.) |
| `examples/fl_r7_fuzz/evil_worker.cpp` (FL r7) | **Retired 2026-05 — no longer built.** This was a fuzz harness for the host-side IPC reader, mimicking `xinsp-worker.exe` on the wire (named-pipe client). The `evil_worker` CMake target was removed and the worker IPC it fuzzed is gone with the SHM/process-isolation removal, so the harness is dead code; not a Linux port concern. |
| `examples/fl_r7_fuzz/harness_*.py` + `_common.py` (FL r7) | Pure Python (websocket-client + stdlib). No Win32 dependency. The backend-spawn helper relies on `subprocess.Popen` with no platform-specific flags. |
| `examples/fl_r8_concurrency/harness_*.py` + `_common.py` (FL r8) | Pure Python; reuses r7's `BackendProc`. The Win-specific bits — `tasklist` (process count, RSS, pid lookup) and `taskkill /F` (used by `harness_backend_kill.py` to verify worker cleanup on host death) — are isolated in `_common.py` behind `os.name == "nt"` branches with `TODO(linux)` comments pointing at `pgrep` / `/proc/<pid>/statm` / `os.kill(pid, 9)` fallbacks. The kill path includes a name-check that refuses to kill anything other than `xinsp-backend.exe` / `xinsp-worker.exe` (per the operator-safety memory rule). On Linux those checks become a `/proc/<pid>/comm` read. |

## Things to actively reduce Win-coupling for, even before the port

- `cl.exe` mojibake on CP-950 — already worked around with `VSLANG=1033`,
  but `g++`/`clang++` paths just give utf-8 output; the workaround is
  Windows-specific scaffolding the Linux port can drop entirely.
- `xi_thread.hpp` exists for SEH-installing thread spawn; on Linux we
  can collapse it to plain `std::thread` + per-thread signal mask.
- The `extra_plugin_dirs` parsing assumes Windows path separators in a
  few places (search for `;` joiners). Use `std::filesystem::path`
  + the OS's preferred separator instead.

## Work-size estimate (when actually porting)

| Scope | Estimate |
|---|---|
| Pure backend port (Easy + Medium tables above) | 6-10 person-days |
| Full platform parity including e2e, extension, SDK templates | + 3-5 days |
| Validation: re-run all 12 backend ctest targets + e2e suites on Linux | + 2 days |
| Buffer for discovery (every port has surprises) | + 30% |

Roughly **2-3 weeks** for an end-to-end port if the porter knows both
platforms; longer if learning. The removal of process isolation +
the SHM/IPC mesh (2026-05) makes the port *simpler*: the
worker / script-runner / shared-memory layer — previously the hardest,
most Win-API-coupled part of the backend — no longer exists. The
remaining Win-coupling is the smaller surface in the tables above
(`dlopen`, sockets, SEH, the compile driver, crash forensics).

---

## Effort + phasing

Sizing (single competent dev, native build on the target — not cross-compile).
Numbers are for getting each layer *working + smoke-tested*, not hardened.

| Phase | Scope | Estimate |
|---|---|---|
| **0. Build green** | CMake on Linux, swap the Easy-tier API shims (sockets, `dlopen`, atomic-io, `_strdup`/`MAX_PATH`/wstring). Backend links + boots, WS answers `ping`. | 2–4 days |
| **1. Core loop** | `xi_script_compiler` → `clang++`/`g++` spawn + gcc/clang diagnostic parser; load the compiled `.so` (`dlopen`); a trigger runs one `inspect()`; plugins load. This is the heart — once green, the framework "works" headless. | 1–1.5 weeks |
| **2. Supervisor** | `fe_main.cpp` → `posix_spawn`/`pidfd`/`PR_SET_PDEATHSIG` + signal handlers + POSIX TCP probe. FE respawns the BE on death + records crash history (PLC line-safe is a comms sidecar, not the FE). | 3–5 days |
| **3. Crash forensics** | `xi_seh` → `sigaction`+`siglongjmp` for the per-call net; process-level dumps via **Breakpad/Crashpad** (covers Linux+mac+ARM at once) or, as a stop-gap, core-file + `dladdr` module blame. | 3 days (stop-gap) → 2 weeks (Breakpad) |
| **4. Watchdog** | Redesign — see Hard table. Cooperative checkpoint in the script ABI, or `SIGUSR1`→`siglongjmp` out of the inspect thread. Genuinely a design task, not a port. | 2–4 days design+impl |
| **ARM Linux delta** | Drop IPP (x86-only); confirm OpenCV ARM build + NEON; native or cross toolchain. Otherwise identical POSIX to x64. | +2–4 days |
| **macOS delta** | `.dylib` is ~free via `dlopen`; Mach-exception crash handling (Breakpad covers it); `dladdr` blame; **codesign + notarize** for distribution. | +1–2 weeks |

**Reading:** Linux x64 headless + dev-loop + stop-gap crash ≈ **2–3 weeks**.
\+ ARM ≈ a few days. + macOS ≈ 1–2 weeks. Full Breakpad parity + watchdog
redesign across all three: add ~2–3 weeks on top. The VS Code extension and
Python SDK are already portable (minus the PowerShell screenshot e2e bits in the
"Outside the backend" table) — effectively free.

## ARM & macOS deltas (beyond the Linux tables)

- **ARM (Linux or macOS).** Pure-C++ `xi_*` code is arch-neutral. The one real
  change: **Intel IPP is x86/x64 only — drop it on ARM** (the `XINSP2_HAS_IPP`
  path) and lean on OpenCV's own NEON-accelerated ops. libjpeg-turbo has NEON, so
  the turbojpeg path is fine. Make sure the OpenCV you link/ship is an ARM build.
- **ARM encode acceleration — measured (Raspberry Pi 5, Cortex-A76).** There is
  **NO hardware video/JPEG *encoder* on the Pi 5**: VideoCore VII dropped the
  encode block the Pi 4 had (`vcgencmd codec_enabled` → H264/H265/JPG all
  `disabled`); the only V4L2 codec device is `rpi-hevc-dec` (a *decoder*), plus the
  PiSP ISP (`pispbe-*`, camera debayer/denoise/HDR — not an encoder). So the encode
  hot path (preview egress) is CPU-bound. This is why the upstream `doc 36`
  hardware-video-encode investigation is DEFERRED. Two SOFTWARE levers, both
  benchmarked on this Pi 5:
  - **libjpeg-turbo (NEON):** ~3.2x vs stb (231 vs 74 MP/s @ 1080p q85), identical
    size/quality — build the imgcodec plugin with `-DXINSP2_HAS_TURBOJPEG=ON`.
  - **frame-level multithread:** turbojpeg is per-`tjhandle` thread-safe and scales
    **perfectly linearly to 3 threads (3.0x)** — identical ratio at 1080p and 4K,
    so it is NOT memory-bandwidth-bound; the 4th thread only reaches ~84% (it
    contends with the OS/dispatch for the 4th core). Sweet spot: **3 encode threads
    (~500 MP/s aggregate), leaving one core for dispatch/serving**.
  - **Combined ~7–9x** aggregate vs stb-single-core — the software substitute for
    the missing HW encoder, and turnkey (a V3D GPU shader encoder is the only other
    avenue and is unexplored/high-effort). The dispatch multi-lane model already
    gives frame-level parallelism, so this falls out naturally.
- **macOS.** `dlopen` loads `.dylib` natively (the loader layer is nearly free).
  Default compiler is clang (friendlier for the compile-driver abstraction than
  juggling MSVC). The cost centres are (a) **crash handling via Mach exception
  ports** (don't hand-roll — use Crashpad), (b) no `/proc`, so module-blame uses
  `dladdr`, and (c) **code signing + notarization** to distribute outside a dev
  box — an Apple Developer account and a non-trivial packaging step, more
  bureaucracy than engineering.
- **Linux x64 ↔ ARM Linux** are essentially the same POSIX target; the delta is
  just IPP + the OpenCV/toolchain arch. Treat x64 as the reference port and ARM
  as a build-matrix variant, not a separate effort.

## The runtime-compile constraint + AOT bundle strategy

The single fact that dominates deployment: xInsp2's value (HDevelop-style
iteration) comes from **compiling the inspection script + project plugins at
runtime**. So every machine that *opens/edits* a project needs a working C++
toolchain + OpenCV dev headers present — this is not a "ship one binary" port.

- **Upside on Linux/mac:** installing the toolchain is *easier* than Windows MSVC
  (`apt install g++` / `xcode-select` / `brew`). The `toolchain_health` command +
  a `setup-linux.sh` / `setup-macos.sh` (mirroring `tools/setup-windows.ps1`) port
  over directly. The per-project `c_cpp_properties.json` generation should emit a
  `linux-clang-x64` / `linux-gcc-arm64` IntelliSense config instead of
  `windows-msvc-x64` (already flagged `TODO(linux)` in `service_main.cpp`).
- **Locked / embedded ARM devices** usually should NOT carry a compiler. The lever:
  add an **AOT / pre-compiled bundle mode** — compile the script + plugins to
  `.so` once on a dev/CI box, ship the binaries, and have the device *load only,
  never compile*. This is the cross-platform generalisation of the existing
  "locked production line ships pre-compiled DLLs" note, and it roughly halves the
  embedded-deployment burden (no toolchain, no OpenCV headers, smaller image). Worth
  designing the bundle format (a manifest + `.so` set + cert) before the port so
  the loader path is built for it from day one.

## Include-tree split — the SDK public-root design (execute WITH the port)

The core-minimisation campaign (2026-06) separated the `xi` headers into three
*concerns*. Two of the three are already physically split; the third is designed
here and deliberately deferred to the port because it touches the same
runtime-compile machinery the Linux compile-driver work has to generalise anyway
— doing it Windows-only now then re-doing it at port time is wasted churn.

**Done (source + build, compile-enforced):**
- `fe/include/xi/` — the FE-supervisor surface (`xi_crash_history`,
  `xi_crash_report`, `xi_fe_status`, `xi_respawn_policy`).
  Reached **only** by `fe_main.cpp` + the three `test_qa_*` units, via the
  `xi_fe` CMake INTERFACE lib. It is **not** on `xi_core`'s include path, so the
  backend/runner binaries and the runtime script/plugin cl-compiler physically
  cannot include FE headers — the split is enforced by the compiler, not just by
  convention. New supervisor-only headers go here, not in `backend/include/xi/`.

**Deferred to the port — the SDK public root:**
The blocker is a load-bearing invariant: the runtime cl-compiler resolves a
**single** include dir (`service_main.cpp` walks up from the exe for
`include/xi/xi.hpp`, then passes that one dir as `/I` to every script + plugin
compile; `vendor_dir` and the plugin-cmake `XINSP2_ROOT` are *derived* from it).
Everything a script/plugin needs therefore has to live under one flat `xi/`.

The correct split is **not** "move the 19 SDK-only headers out". A plugin's
public surface is the SDK headers (`xi.hpp`, `xi_var/param/io/cv/...`) **plus the
core ABI/data substrate they include** — `xi_abi.{h,hpp}`, `xi_image`,
`xi_record`, `xi_image_pool`, `xi_instance`, `xi_doc_pool`, `xi_doc_registry`,
`xi_seh`, `xi_cabi_adapter`, and their transitive deps (the 20 SDK→CORE edges).
So:

- `sdk/include/xi/` = **SDK headers + the public ABI substrate = self-contained.**
  A plugin/script compile (every platform, via clang/g++/cl) points at this ONE
  root and needs nothing else. The 20 SDK→CORE edges stop crossing a boundary
  because the substrate moves with the SDK.
- `backend/include/xi/` = host-internal only (`xi_plugin_manager`,
  `xi_script_compiler`, `xi_ws_server`, `xi_trigger_bus`, `xi_project`,
  `xi_protocol`, `xi_use`, `xi_pm_*`, `xi_status_sink`, `xi_sha256`,
  `xi_cli_args`, `xi_jpeg`). These include the public substrate, so the host
  build adds `-I sdk/include` — but the host is built by CMake we control.
- `fe/include/xi/` = supervisor (already done).

Port-time execution checklist (all of it aligns with the Linux compile-driver
work, which has to touch these same sites):
1. Generalise `CompileEnv.include_dir` (single) → an **include-dir list**; the
   clang/g++ spawn and the `cl.exe` spawn both emit one `-I`/`/I` per entry.
   `service_main`'s probe finds `sdk/include/xi/xi.hpp` and seeds the list with
   `sdk/include` (+ the host adds `backend/include` for its own build only).
2. Rewrite the 20 SDK→CORE relative includes (`"xi_foo.hpp"`) to angle form
   (`<xi/xi_foo.hpp>`) so they resolve via the `-I` search path regardless of
   which root each header sits in. (List: `xi.hpp`, `xi_async`, `xi_cv`, `xi_io`,
   `xi_plugin_support`, `xi_script_support`, `xi_state`,
   `xi_thread`, `xi_types`, `xi_var` → into `xi_image`/`xi_record`/`xi_abi.h`/
   `xi_instance`/`xi_image_pool`/`xi_seh`.)
3. `sdk/cmake/xinsp2_plugin.cmake`: `XINSP2_INCLUDE` → `sdk/include` (was
   `backend/include`); drop the derived `backend/include` assumption.
4. `tools/build_release.mjs`: the release currently ships `sdk/` + `bin/` but
   **never stages `backend/include` as a runtime `include/`** — so once the
   public root is `sdk/include/xi/`, the release naturally ships the right
   headers and the deployed runtime probe finds them. Confirm the staged layout
   puts `sdk/include/xi/xi.hpp` where `service_main`'s walk-up expects it.
5. Re-verify EVERY cl/clang-driven compile path: script compile, project-plugin
   compile, `build:cmake` plugin, AOT bundle, `rebuild_plugins`.

Net: one self-contained `sdk/include/xi/` public root that the cross-platform
plugin toolchain points at, host-internal headers invisible to plugins, FE
already walled off. Until then, Step 2a already removed the SDK headers from the
backend *binary*'s own compile (the umbrella `#include <xi/xi.hpp>` is gone), so
the binary is already minimal — only the source-tree relocation remains.

## See also

- `docs/testing.md` "Known limitations" already calls out the Linux
  build path as untested.
- `docs/archive/ipc-shm.md` — IPC + SHM design (historical; the
  cross-process work was removed 2026-05).
- `docs/overview.md` — top-level component map; everything not in
  the tables above is presumed portable.
