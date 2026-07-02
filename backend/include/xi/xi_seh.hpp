#pragma once
//
// xi_seh.hpp — convert Windows SEH (access violation, divide-by-zero, etc.)
// into C++ exceptions catchable by ordinary try/catch.
//
// Install once per worker thread (the translator is per-thread on MSVC):
//
//     _set_se_translator(xi::seh_translator);
//
// Then:
//
//     try { plugin_call(); }
//     catch (const xi::seh_exception& e) {
//         // e.code is the Win32 SEH code, e.what() is a short name
//     }
//
// Helpers `xi::spawn_worker` (xi_thread.hpp) and `xi::async` (xi_async.hpp)
// install this translator automatically at thread entry.
//
// MSVC-only — no-op everywhere else.
//

#include <cstdio>
#include <cstdlib>
#include <exception>

#ifdef _MSC_VER
#include <eh.h>
#include <malloc.h>   // _resetstkoflw
#endif

namespace xi {

class seh_exception : public std::exception {
public:
    unsigned int code;
    explicit seh_exception(unsigned int c) : code(c) {}
    const char* what() const noexcept override {
        switch (code) {
            case 0xC0000005: return "ACCESS_VIOLATION";
            case 0xC0000094: return "INT_DIVIDE_BY_ZERO";
            case 0xC000008C: return "ARRAY_BOUNDS_EXCEEDED";
            case 0xC00000FD: return "STACK_OVERFLOW";
            case 0xC000001D: return "ILLEGAL_INSTRUCTION";
            case 0xC0000090: return "FLOAT_INVALID_OPERATION";
            case 0xC0000091: return "FLOAT_DIVIDE_BY_ZERO";
            case 0xC0000096: return "PRIVILEGED_INSTRUCTION";
            case 0xC00000FE: return "INVALID_DISPOSITION";
            default:         return "UNKNOWN_SEH_EXCEPTION";
        }
    }
};

#ifdef _MSC_VER
inline void seh_translator(unsigned int code, struct _EXCEPTION_POINTERS*) {
    throw seh_exception(code);
}

inline void install_seh_translator() {
    _set_se_translator(seh_translator);
}
#else
inline void install_seh_translator() {}
#endif

// EXCEPTION_STACK_OVERFLOW, spelled here so this header stays free of <windows.h>.
inline constexpr unsigned int kStackOverflowCode = 0xC00000FD;

// Recover the thread's stack state after a translated seh_exception is CAUGHT on a
// thread that will KEEP RUNNING (a pooled dispatch/OpenMP/async worker, the WS
// command thread, the runner's frame loop).
//
// Only STACK_OVERFLOW needs anything: the overflow is delivered by writing into the
// thread's single guard page, and Windows does NOT re-arm that page automatically.
// Until _resetstkoflw() restores it, the NEXT deep call on this thread does not
// fault cleanly — it writes past the end of the stack and silently corrupts memory
// (or dies unrecoverably). Every other SEH code leaves the stack intact, so this is
// a no-op for them. No-op returning true off MSVC.
//
// Returns true when the thread is safe to keep using (not an overflow, or the guard
// page was restored); false when the guard page could NOT be restored — the caller
// MUST NOT run more untrusted code on this thread.
inline bool recover_seh_stack(unsigned int code) {
#ifdef _MSC_VER
    if (code == kStackOverflowCode)
        return _resetstkoflw() != 0;
#else
    (void)code;
#endif
    return true;
}
inline bool recover_seh_stack(const seh_exception& e) { return recover_seh_stack(e.code); }

// Exit code for a self-inflicted hard exit that the FE supervisor respawns. Matches
// service_internal.hpp's WATCHDOG_EXIT_CODE (0x5744 == 'WD'); duplicated here so the
// shared xi_parallel / xi_async workers can hard-exit without a service dependency.
inline constexpr int kStackGuardExitCode = 0x5744;

// recover_seh_stack(), plus the unrecoverable-guard-page policy: if the guard page
// cannot be restored the thread's stack is compromised and cannot be run again, so
// we hard-exit crash-safely for the FE supervisor to respawn — the same trade the
// watchdog HARD trip makes (log + std::_Exit, skipping dtors a wedged thread could
// deadlock on). Used at every seh_exception catch site whose thread survives to run
// more untrusted code. On the reset-SUCCESS path it returns and the caller proceeds
// exactly as before (the fault is already recorded / policy already applied).
inline void recover_seh_stack_or_die(unsigned int code, const char* where) {
    if (recover_seh_stack(code)) return;
    std::fprintf(stderr,
        "[xinsp2] STACK_OVERFLOW guard page could not be restored on %s — "
        "hard-exiting for supervisor respawn (rc=0x%04X)\n",
        where ? where : "worker", (unsigned)kStackGuardExitCode);
    std::fflush(stderr);
    std::fflush(stdout);
    std::_Exit(kStackGuardExitCode);
}
inline void recover_seh_stack_or_die(const seh_exception& e, const char* where) {
    recover_seh_stack_or_die(e.code, where);
}

} // namespace xi
