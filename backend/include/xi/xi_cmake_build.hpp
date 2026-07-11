#pragma once
//
// xi_cmake_build.hpp — host-side cmake invocation for "build": "cmake" plugins.
//
// A plugin with `"build": "cmake"` owns its own build (its CMakeLists handles
// external libs / CUDA); the backend shells out to cmake to configure + build
// it rather than cl.exe-compiling the source itself. These are the pure process
// + filesystem mechanics of that: scan a source tree's newest mtime (the
// rebuild change-gate), capture a child process's output, and configure+build
// one plugin. The PluginManager keeps the stateful orchestration
// (rebuild_cmake_plugins: which plugins, unload/reload, restore instances).
//
// Extracted from xi_plugin_manager.hpp — no PluginManager state, so a leaf the
// manager calls into.
//
// TODO(linux): run_cmd_capture has the _WIN32 CreateProcess path; the popen()
// branch is stubbed pending the Linux port (see linux-port.md) — when written,
// it must carry the same timeout bound (kCmakeBuildTimeoutMs + kill).
//
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
#  include <sys/wait.h>   // WIFEXITED / WEXITSTATUS for popen'd cmake
#endif

namespace xi {
namespace cmake_build {

// Newest write-time (ticks) over a cmake plugin's source tree — the rebuild
// change-gate input. Counts source + build-script files, skips the build/ tree
// (its artifacts aren't "source"). 0 if nothing found.
inline uint64_t newest_source_mtime(const std::string& dir) {
    uint64_t newest = 0;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return 0;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory()) {
            if (it->path().filename() == "build") it.disable_recursion_pending();
            continue;
        }
        auto ext = it->path().extension().string();
        auto fn  = it->path().filename().string();
        bool src = ext == ".c"  || ext == ".cpp" || ext == ".cc"  || ext == ".cxx" ||
                   ext == ".cu" || ext == ".cuh" || ext == ".h"   || ext == ".hpp" ||
                   ext == ".hxx"|| fn == "CMakeLists.txt" || ext == ".cmake";
        if (!src) continue;
        auto wt = std::filesystem::last_write_time(it->path(), ec);
        if (!ec) newest = std::max(newest, (uint64_t)wt.time_since_epoch().count());
    }
    return newest;
}

#ifdef _WIN32
// Ceiling on one cmake configure/build invocation. rebuild runs on the sole
// poll thread while holding the quiesce + manager lock, so a wedged toolchain
// (hung MSBuild node, AV-locked link output, stuck nvcc) must NOT freeze the
// control plane indefinitely — same rationale as the script compiler's
// kCompileTimeoutMs, but larger: a cmake plugin build is a full configure +
// multi-TU (possibly CUDA) build, not a single cl.exe run.
static constexpr DWORD kCmakeBuildTimeoutMs = 600000;   // 10 min
#endif

// Run a command, capturing combined stdout+stderr into *log. Returns the
// process exit code, -1 if it couldn't be spawned, or -2 if it exceeded
// kCmakeBuildTimeoutMs (the whole child tree is killed and the timeout is
// noted in *log, so the caller reports a failed build instead of hanging).
inline int run_cmd_capture(const std::string& cmd, std::string& log) {
#ifdef _WIN32
    // Same cmd.exe routing _popen used; the outer quotes keep a quoted exe
    // path + quoted args parsing correctly. stdout and stderr are combined by
    // handing both the same pipe write end (no `2>&1` needed).
    std::string full = "cmd.exe /C \"" + cmd + "\"";

    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        log += "[failed to spawn: " + cmd + "]\n"; return -1;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);   // child gets wr only

    // Kill-on-close job object so a timeout reaps the WHOLE tree — cmd ->
    // cmake -> MSBuild -> cl/nvcc — not just the immediate cmd.exe (the gap
    // run_with_env in xi_script_compiler.hpp still has). Created suspended so
    // the child is inside the job before it can spawn anything.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    std::vector<char> mut(full.begin(), full.end()); mut.push_back('\0');
    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError  = wr;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, mut.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        if (job) CloseHandle(job);
        CloseHandle(rd); CloseHandle(wr);
        log += "[failed to spawn: " + cmd + "]\n"; return -1;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);   // best-effort; else pi-only kill below
    ResumeThread(pi.hThread);
    CloseHandle(wr);   // parent's copy — children keep theirs until they exit

    // Drain the pipe WHILE waiting (a full 4K pipe buffer would block the
    // child forever — the drain is part of the liveness fix, not just
    // capture), and enforce the wall-clock bound. PeekNamedPipe keeps every
    // read non-blocking: a lingering grandchild (mspdbsrv, an MSBuild reuse
    // node) holding the write end must not turn the final read into a hang.
    auto drain = [&]() {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[4096]; DWORD got = 0;
            DWORD want = avail < (DWORD)sizeof(buf) ? avail : (DWORD)sizeof(buf);
            if (!ReadFile(rd, buf, want, &got, nullptr) || got == 0) break;
            log.append(buf, got);
        }
    };
    ULONGLONG start = GetTickCount64();
    bool timed_out = false;
    for (;;) {
        drain();
        if (WaitForSingleObject(pi.hProcess, 50) != WAIT_TIMEOUT) break;   // exited
        if (GetTickCount64() - start >= kCmakeBuildTimeoutMs) { timed_out = true; break; }
    }

    int rc;
    if (timed_out) {
        // A hung toolchain is as dangerous to the control plane as a crashed
        // one: kill the tree and surface a failed build.
        if (job) TerminateJobObject(job, 1);
        else     TerminateProcess(pi.hProcess, 1);   // no job: immediate child only
        WaitForSingleObject(pi.hProcess, 2000);
        drain();
        log += "[timed out after " + std::to_string(kCmakeBuildTimeoutMs / 1000) +
               "s — killed: " + cmd + "]\n";
        rc = -2;
    } else {
        drain();   // tail written between the last in-loop drain and exit
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        rc = (int)code;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);   // kill-on-close also reaps any straggler nodes
    CloseHandle(rd);
    return rc;
#else
    // popen routes through /bin/sh; combined stream via 2>&1, no outer-quote wrap
    // (the Windows form needs it for cmd.exe's quoted-exe parsing; sh doesn't).
    // TODO(linux): not yet bounded like the _WIN32 path (kCmakeBuildTimeoutMs +
    // kill-the-tree). A wedged toolchain here still stalls the control plane; a
    // fork/exec + poll-timeout + killpg version (cf. certify's fork+waitpid) is
    // the follow-up.
    std::string full = cmd + " 2>&1";
    FILE* pipe = ::popen(full.c_str(), "r");
    if (!pipe) { log += "[failed to spawn: " + cmd + "]\n"; return -1; }
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) log += buf;
    int rc = ::pclose(pipe);
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 128;
#endif
}

// Configure (first time) + build one cmake plugin. Appends logs. 0 = success.
// `opencv_dir` is the host's top-level OpenCV pack dir (may be empty).
inline int build_cmake_plugin(const std::string& cmake_exe, const std::string& src_dir,
                              const std::string& config, const std::string& xinsp_root,
                              const std::string& opencv_dir, std::string& log) {
    auto build_dir = (std::filesystem::path(src_dir) / "build").string();
    // The command goes through cmd.exe (run_cmd_capture's `cmd.exe /C`).
    // `cmake_exe` and `config` come from the
    // WS client, so reject shell metacharacters (and a `"` that could
    // break out of quoting) before they reach the shell — command-injection
    // guard, mirrors the cl.exe driver's is_safe_path. Paths are quoted below;
    // Windows paths can't contain `"` and `&`/`^` are inert inside quotes.
    auto unsafe = [](const std::string& s) {
        return s.find_first_of("&|<>^%!\"`\n\r") != std::string::npos;
    };
    if (unsafe(cmake_exe) || unsafe(config)) {
        log += "[refused: cmake_exe/config contains shell metacharacters]\n";
        return -1;
    }
    auto q = [](const std::string& s) { return "\"" + s + "\""; };
    if (!std::filesystem::exists(std::filesystem::path(build_dir) / "CMakeCache.txt")) {
        std::string cfg = q(cmake_exe) + " -S " + q(src_dir) + " -B " + q(build_dir) +
                          " -DXINSP2_ROOT=" + q(xinsp_root);
#ifdef _WIN32
        // VS multi-config generator: select the x64 platform at configure.
        cfg += " -A x64";
        // OpenCV: pass the resolved `x64/vcNN/lib` SUBDIR (the level whose
        // OpenCVConfig.cmake actually resolves), NOT the top-level pack dir —
        // forcing the top dir makes OpenCV's runtime auto-detect set
        // OpenCV_FOUND=FALSE. opencv_dir is the host's top pack dir; derive the
        // vc subdir so a NON-standard OpenCV location also works. If none
        // matches, omit it and let xinsp2_plugin.cmake's own (hard-coded
        // C:/opencv) probe try.
        if (!opencv_dir.empty()) {
            for (const char* vc : {"vc17", "vc16", "vc15", "vc14"}) {
                auto lib = std::filesystem::path(opencv_dir) / "x64" / vc / "lib";
                if (std::filesystem::exists(lib / "OpenCVConfig.cmake")) {
                    cfg += " -DOpenCV_DIR=" + q(lib.string());
                    break;
                }
            }
        }
#else
        // POSIX: single-config generator (Make/Ninja) picks the build type at
        // configure, not build; no `-A` (that is VS-only). OpenCV resolves via
        // the system CMake package (libopencv-dev) or the OpenCV_DIR env var, so
        // opencv_dir (a Windows pack layout) is not forced. If it happens to point
        // at a dir holding OpenCVConfig.cmake, honour it.
        cfg += " -DCMAKE_BUILD_TYPE=" + config;
        if (!opencv_dir.empty() &&
            std::filesystem::exists(std::filesystem::path(opencv_dir) / "OpenCVConfig.cmake"))
            cfg += " -DOpenCV_DIR=" + q(opencv_dir);
#endif
        log += "[configure] " + cfg + "\n";
        int rc = run_cmd_capture(cfg, log);
        if (rc != 0) return rc;
    }
    // --config is honoured by multi-config generators (VS) and ignored (with a
    // benign note) by single-config ones, where the build type was set above.
    std::string bld = q(cmake_exe) + " --build " + q(build_dir) + " --config " + q(config);
    log += "[build] " + bld + "\n";
    return run_cmd_capture(bld, log);
}

} // namespace cmake_build
} // namespace xi
