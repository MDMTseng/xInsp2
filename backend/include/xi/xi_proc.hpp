#pragma once
//
// xi_proc.hpp — the ONE bounded win32 process spawn (leaf: std + win32 only).
//
// Round-3 #6 consolidation. Root cause: the tree had TWO spawn shapes and a
// third, unbounded one:
//   * xi_cmake_build.hpp run_cmd_capture — the correct shape (CreateProcess +
//     kill-on-close job object + wall-clock timeout + non-blocking pipe drain);
//   * xi_script_compiler.hpp run_with_env — bounded, but on timeout killed only
//     the immediate cmd.exe: orphaned cl.exe/mspdbsrv grandchildren kept the
//     .dll/.pdb locked (the file itself admitted the gap);
//   * BOTH cl.exe compile paths fell back to UNBOUNDED std::system when the
//     vcvars env capture failed — a wedged cl then pinned the poll thread
//     forever, the exact class the 300s timeout was added to kill.
// This header owns the spawn; the callers keep their own timeout constants,
// env blocks and cmd.exe wrapping. The std::system fallbacks are DELETED: when
// no env block is available we spawn with the parent's environment inherited
// (env == nullptr) — a plain bounded spawn replaces std::system 1:1 because
// those command lines already start with `cmd /C` and route vcvars inline.
//
// POSIX lane: unchanged (popen-based, lives with its callers) — this is the
// win32 lane only.

#include <string>
#include <vector>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

namespace xi {
namespace proc {

// Run `cmdline` (passed to CreateProcess verbatim — include your own
// `cmd /C ...` wrapping if shell routing is wanted), bounded by `timeout_ms`.
//
//   env     — CreateProcess ANSI environment block (double-null terminated),
//             or nullptr to inherit the parent's environment.
//   capture — if non-null, combined stdout+stderr is appended here (stdin is
//             inherited); if null, the child inherits the parent's std handles
//             (callers redirect inside the command line as before).
//
// Returns the child's exit code, -1 if it couldn't be spawned, or -2 on
// timeout. On timeout the WHOLE child tree is killed via the kill-on-close job
// object (cmd -> cl/cmake -> MSBuild/mspdbsrv/nvcc), never just the immediate
// child. Spawn-failure / timeout notes are appended to *capture when present.
inline int spawn_bounded(const std::string& cmdline, DWORD timeout_ms,
                         const std::vector<char>* env, std::string* capture) {
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (capture) {
        // stdout and stderr are combined by handing both the same pipe write
        // end (no `2>&1` needed).
        if (!CreatePipe(&rd, &wr, &sa, 0)) {
            *capture += "[failed to spawn: " + cmdline + "]\n";
            return -1;
        }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);   // child gets wr only
    }

    // Kill-on-close job object so a timeout reaps the WHOLE tree, not just the
    // immediate cmd.exe. Created before the child, and the child starts
    // SUSPENDED so it is inside the job before it can spawn anything.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    std::vector<char> mut(cmdline.begin(), cmdline.end()); mut.push_back('\0');
    STARTUPINFOA si{}; si.cb = sizeof(si);
    if (capture) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = wr;
        si.hStdError  = wr;
    }
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, mut.data(), nullptr, nullptr,
                        /*bInheritHandles=*/capture ? TRUE : FALSE,
                        CREATE_SUSPENDED,
                        env ? (LPVOID)env->data() : nullptr, nullptr, &si, &pi)) {
        if (job) CloseHandle(job);
        if (capture) {
            CloseHandle(rd); CloseHandle(wr);
            *capture += "[failed to spawn: " + cmdline + "]\n";
        }
        return -1;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);   // best-effort; else pi-only kill below
    ResumeThread(pi.hThread);
    if (capture) CloseHandle(wr);   // parent's copy — children keep theirs until they exit

    // Drain the pipe WHILE waiting (a full 4K pipe buffer would block the
    // child forever — the drain is part of the liveness fix, not just
    // capture), and enforce the wall-clock bound. PeekNamedPipe keeps every
    // read non-blocking: a lingering grandchild (mspdbsrv, an MSBuild reuse
    // node) holding the write end must not turn the final read into a hang.
    auto drain = [&]() {
        if (!capture) return;
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[4096]; DWORD got = 0;
            DWORD want = avail < (DWORD)sizeof(buf) ? avail : (DWORD)sizeof(buf);
            if (!ReadFile(rd, buf, want, &got, nullptr) || got == 0) break;
            capture->append(buf, got);
        }
    };
    ULONGLONG start = GetTickCount64();
    bool timed_out = false;
    for (;;) {
        drain();
        if (WaitForSingleObject(pi.hProcess, 50) != WAIT_TIMEOUT) break;   // exited
        if (GetTickCount64() - start >= timeout_ms) { timed_out = true; break; }
    }

    int rc;
    if (timed_out) {
        // A hung toolchain is as dangerous to the control plane as a crashed
        // one: kill the tree and surface the timeout.
        if (job) TerminateJobObject(job, 1);
        else     TerminateProcess(pi.hProcess, 1);   // no job: immediate child only
        WaitForSingleObject(pi.hProcess, 2000);
        drain();
        if (capture)
            *capture += "[timed out after " + std::to_string(timeout_ms / 1000) +
                        "s — killed: " + cmdline + "]\n";
        rc = -2;
    } else {
        drain();   // tail written between the last in-loop drain and exit
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        rc = (int)code;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);   // kill-on-close also reaps any straggler nodes
    if (capture) CloseHandle(rd);
    return rc;
}

} // namespace proc
} // namespace xi

#endif // _WIN32
