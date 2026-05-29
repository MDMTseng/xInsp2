# Linux port — inventory + going-forward rule

> **Status: NOT scheduled.** This is a parking lot for "things that
> need to change when we eventually port to Linux" so we don't lose
> track of them, plus a rule for new code to follow now.

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
| `backend/src/fe_main.cpp` (the `xinsp-fe` supervisor) | `CreateProcessA` + `WaitForSingleObject`, Job Object (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), `SetConsoleCtrlHandler`, Winsock TCP probe | `posix_spawn`/`fork`+`execv`; `waitpid`/`pidfd`; `prctl(PR_SET_PDEATHSIG)` for the kill-on-parent-death guarantee; `sigaction(SIGINT/SIGTERM)`; POSIX `connect` probe. The whole Win32 path is already gated `#ifdef _WIN32` with a `TODO(linux)` stub `main()`; `xi_safe_state.hpp` + the crash-log parsing are portable. |

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

## See also

- `docs/testing.md` "Known limitations" already calls out the Linux
  build path as untested.
- `docs/reference/ipc-shm.md` — IPC + SHM design (historical; the
  cross-process work was removed 2026-05).
- `docs/architecture.md` — top-level component map; everything not in
  the tables above is presumed portable.
