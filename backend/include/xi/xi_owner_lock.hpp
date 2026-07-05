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

// Is `pid` a currently-running process?
//
// K11 — the two platforms resolve the "live process owned by ANOTHER user"
// ambiguity in OPPOSITE directions, and that divergence is deliberate-but-honest,
// not a single uniform policy:
//   * Windows: ANY OpenProcess failure — including ERROR_ACCESS_DENIED on a LIVE
//     process owned by a different user — is treated as DEAD ⇒ the stamp is
//     reclaimed. (We only get a handle, hence only vote "alive", on positive
//     access.) So a cross-user live owner can be silently taken over.
//   * Linux: kill(pid,0) returning EPERM (the process EXISTS but is owned by
//     another user) is treated as ALIVE ⇒ the stamp is NOT reclaimed.
// This is advisory-only (we warn, never refuse; a stale stamp is always
// eventually reclaimable), so the split is tolerated rather than unified — but it
// IS a real per-platform behavior difference, documented here so nobody reads the
// two branches expecting them to agree.
//
// K12 — pid-reuse ABA is NOT handled. If a crashed backend's pid is recycled by
// the OS to an unrelated process, pid_alive(old_pid) returns true and read()
// below surfaces a false "project already open elsewhere" warning. The stamp
// carries ts_ms (wall-ms at open) which is NOT currently used to disambiguate a
// reuse — it could, in future, be cross-checked against the recycled process's
// start time to reject an ABA. Advisory-only impact: the lock warns, never
// refuses, so a false positive costs a spurious warning, never data loss.
inline bool pid_alive(uint64_t pid) {
    if (pid == 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return false;                       // K11: ANY failure (gone OR access-denied
                                                //   on a live cross-user process) → dead
    DWORD w = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return w == WAIT_TIMEOUT;                    // not signalled → still running
#else
    // K11: kill(pid,0) → 0 if alive; EPERM means the process EXISTS but is owned
    // by another user → we treat that as ALIVE (opposite of the Windows branch).
    return ::kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}

// ts_ms is the wall-ms at open. K12: it is NOT currently consulted to disambiguate
// a recycled pid (see pid_alive above) — it is carried for diagnostics and as the
// hook a future ABA guard could use.
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
