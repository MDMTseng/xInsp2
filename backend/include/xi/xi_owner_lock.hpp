#pragma once
//
// xi_owner_lock.hpp — advisory single-writer stamp for a project folder (F5).
//
// Opening a project for editing (especially a working copy) rebases writes by
// CONVENTION only — nothing stopped a second backend (or the same project opened
// directly elsewhere) from writing the canonical concurrently, and a later
// working-copy commit's mirror would clobber those writes with no warning.
//
// On open we drop a `.xinsp_owner` stamp (pid + wall-ms) at the canonical root and,
// if one is already there held by a DIFFERENT *live* process, surface a warning.
// Strictly ADVISORY: a stale stamp (the owning pid is gone — crashed backend) is
// silently taken over, so a crash never wedges the project; we warn, never refuse.
//
// Platform-coupled (process-liveness probe), so it lives in its own leaf rather
// than in std-only xi_working_copy.hpp.
//
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <csignal>
#  include <cerrno>
#  include <unistd.h>
#endif

namespace xi {
namespace ownerlock {

// Lives at the canonical project root. Excluded from the working-copy seed/commit
// (xi::wc::is_excluded) so it never rides into the scratch or back onto canonical.
inline constexpr const char* kOwnerFile = ".xinsp_owner";

inline uint64_t self_pid() {
#ifdef _WIN32
    return (uint64_t)GetCurrentProcessId();
#else
    return (uint64_t)::getpid();
#endif
}

// Is `pid` a currently-running process? Advisory: on any ambiguity we lean toward
// "alive" only when we have positive evidence, so a stale stamp is reclaimed rather
// than a dead pid blocking forever.
inline bool pid_alive(uint64_t pid) {
    if (pid == 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return false;                       // no such process (or gone) → treat as dead
    DWORD w = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return w == WAIT_TIMEOUT;                    // not signalled → still running
#else
    // TODO(linux): kill(pid,0) → 0 if alive; EPERM means alive but owned by another user.
    return ::kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}

struct Owner { bool present = false; uint64_t pid = 0; int64_t ts_ms = 0; };

inline Owner read(const std::filesystem::path& canon) {
    Owner o;
    std::ifstream f(canon / kOwnerFile);
    if (!f) return o;
    if (f >> o.pid >> o.ts_ms) o.present = true;
    return o;
}

// Stamp this process as the owner. Best-effort; a write failure (read-only dir)
// just means no advisory protection — never fatal.
inline void write(const std::filesystem::path& canon, int64_t ts_ms) {
    std::ofstream f(canon / kOwnerFile, std::ios::trunc);
    if (f) f << self_pid() << " " << ts_ms << "\n";
}

} // namespace ownerlock
} // namespace xi
