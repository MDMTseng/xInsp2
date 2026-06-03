# Cross-platform port (Linux / ARM / macOS) — inventory, effort + going-forward rule

> **Status: NOT scheduled.** This is a parking lot for "things that
> need to change when we eventually port off Windows" so we don't lose
> track of them, plus a rule for new code to follow now. The actual port
> is done later, on the target platform.
>
> **TL;DR (assessment 2026-06-04).** Moderate, not scary. Only ~15 of 53
> backend files touch Win32 and ~14 sites are already `#ifdef _WIN32`-gated;
> the OS coupling is concentrated, not smeared. A working **Linux x64**
> headless backend (build + load + clang compile + core trigger loop) is
> ~2–3 weeks; **ARM Linux** adds ~2–4 days; **macOS** adds ~1–2 weeks. The
> only genuinely hard items are crash-forensics parity and the watchdog
> redesign. See "Effort + phasing", "ARM & macOS deltas", and "The
> runtime-compile constraint" sections below for the full picture.

---

## Going-forward rule

**Any new code or bug fix from this point on should be cross-platform
unless there's a hard reason it can't be.** That means:

- New Win32 API call site → must have a portable abstraction (or at
  minimum `#ifdef _WIN32` with a TODO for the Linux side).
- New file in `backend/include/xi/` should compile on Linux out of the
  box if it doesn't touch OS primitives. If it does, isolate them
  behind a small header (like `xi_atomic_io.hpp` already does for
  Windows-only via `#ifdef`).
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
| `backend/include/xi/xi_script_loader.hpp` / `xi_plugin_manager.hpp` | `LoadLibraryA` / `GetProcAddress` / `FreeLibrary` | `dlopen` / `dlsym` / `dlclose` |
| `_strdup` / `_dupenv_s` / `_set_se_translator` | MSVC | `strdup` / `getenv` / signal handler |
| `MAX_PATH`, `WCHAR`, `wstring` paths | Win32 | `std::filesystem` already portable; drop wide-char conversions |

### Medium — rewrite a module (1-3 days each)

| File | Win32-only mechanism | Linux equivalent |
|---|---|---|
| `backend/include/xi/xi_seh.hpp` | `_set_se_translator` + `__try`/`__except` | `sigaction(SIGSEGV / SIGFPE / SIGBUS)` + `sigsetjmp`/`siglongjmp`. Or wire Google Breakpad. |
| `backend/include/xi/xi_script_compiler.hpp` | `cl.exe` + `vcvars64.bat` + `cmd /C` | `g++` or `clang++` direct spawn; rewrite the diagnostic parser for gcc / clang error format. |
| Crash forensics in `backend/src/service_main.cpp` | `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` + `EnumProcessModules` + `AddVectoredExceptionHandler` | Google Breakpad or manual `sigaction` + core-file generation; `dl_iterate_phdr` for module-blame addresses. |
| `backend/src/fe_main.cpp` (the `xinsp-fe` supervisor) | `CreateProcessA` + `WaitForSingleObject`, Job Object (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), `SetConsoleCtrlHandler`, Winsock TCP probe. Also spawns the comms gateway (`spawn_gateway`) with the SAME primitives into the SAME job. | `posix_spawn`/`fork`+`execv`; `waitpid`/`pidfd`; `prctl(PR_SET_PDEATHSIG)` for the kill-on-parent-death guarantee; `sigaction(SIGINT/SIGTERM)`; POSIX `connect` probe. The whole Win32 path is already gated `#ifdef _WIN32` with a `TODO(linux)` stub `main()`; `xi_safe_state.hpp` + the crash-log parsing are portable. The gateway spawn reuses the BE spawn path — no new Win32 primitives. |
| `backend/src/comms_main.cpp` (the `xinsp-comms` gateway) and the `GatewayClient` in `backend/src/service_main.cpp` | Winsock (`WSAStartup`, `socket`/`connect`/`select`, `closesocket`) for the loopback BE↔gateway link and the PLC TCP/UDP socket | POSIX BSD sockets (`socket`/`connect`/`select`/`close`), drop `WSAStartup`/`WSACleanup`. Plain socket code, no Win-specific semantics — a thin `#ifdef` shim over the socket calls covers it. `xi_comms.hpp` (the script-facing API) is already portable. |

### Hard — different semantics (re-design)

| Mechanism | Why hard |
|---|---|
| `TerminateThread` watchdog (`backend/src/service_main.cpp`) | Linux has no synchronous "kill thread" primitive. `pthread_cancel` requires cooperative cancellation points the user script won't honour. The earlier "kill the worker process" fix is gone — process isolation was removed 2026-05 and everything runs in-process — so the Linux watchdog story is an open **TODO(linux)**: options include a cooperative-cancellation checkpoint API in the script ABI, or a `SIGUSR`-based interrupt that longjmps out of the inspect thread. |
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
| `examples/multi_source_surge/` (FL r6) | Pure `<thread>` + `<chrono>` + `<atomic>` + the `xi_*` portable headers. No Win32 calls in plugins or inspect. Builds via the same `cl.exe` path the rest of the SDK uses; on Linux it'll go through whatever the script compiler abstracts to. |
| `dispatch_stats` watchdog warning log on `cmd:start` (FL r6) | Pure C++; lives in service_main.cpp's existing log-emission path which is already non-Win-specific. |
| `examples/multi_source_surge2/` (FL r6 regression) | Same plugin sources as `multi_source_surge/`; new `inspect.cpp` + `driver.py` use only `<chrono>`/`<cstdint>`/`<cstring>` + `xi_*` headers + Python stdlib. No Win32. |
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
| Validation: re-run all 16 backend tests + e2e suites on Linux | + 2 days |
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
| **2. Supervisor + comms** | `fe_main.cpp` → `posix_spawn`/`pidfd`/`PR_SET_PDEATHSIG` + signal handlers + POSIX TCP probe; `comms_main.cpp` sockets. FE drives safe-state on BE death. | 3–5 days |
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

## See also

- `docs/testing.md` "Known limitations" already calls out the Linux
  build path as untested.
- `docs/reference/ipc-shm.md` — IPC + SHM design (historical; the
  cross-process work was removed 2026-05).
- `docs/architecture.md` — top-level component map; everything not in
  the tables above is presumed portable.
