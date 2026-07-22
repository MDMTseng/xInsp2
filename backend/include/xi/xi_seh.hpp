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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>

#ifdef _MSC_VER
#include <eh.h>
#include <malloc.h>   // _resetstkoflw
#elif !defined(_WIN32)
#include <csignal>    // sigaction, siginfo_t, SIG* — POSIX fault→exception path
#include <cstdint>    // uintptr_t
#include <cstring>    // memset
#include <vector>     // per-thread alt signal stack
#include <pthread.h>  // pthread_getattr_np — thread stack bounds for overflow classify
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
#elif !defined(_WIN32)
// -------------------------------------------------------------------------
// POSIX equivalent of _set_se_translator: a signal handler for the synchronous
// hardware faults (SIGSEGV/SIGFPE/SIGBUS/SIGILL) that THROWS a seh_exception,
// so the same `try { plugin_call(); } catch (const xi::seh_exception&)` sites
// that catch translated SEH on Windows also catch a segfault/divide-by-zero on
// Linux. This works ONLY because the whole tree (and the JIT-compiled scripts/
// plugins) is built with -fnon-call-exceptions (see CMakeLists): that flag makes
// GCC/Clang emit unwind info for trapping instructions, so the C++ unwinder can
// unwind out of the faulting frame. Without it, the throw hits std::terminate.
//
// The handler is process-global (sigaction is not per-thread), but the THROW
// happens on the faulting thread and unwinds ITS stack, so re-installing it from
// every worker (as the Windows per-thread translator is re-set) is harmless and
// idempotent. SA_NODEFER keeps the faulting signal unblocked as we leave the
// handler via a throw rather than a normal return.
// Low address of the faulting thread's stack, captured at install time (calling
// pthread_getattr_np in the signal handler is not async-signal-safe — it reads
// /proc/self/maps for the main thread — so we record it up front). 0 = unknown.
inline uintptr_t& tls_stack_lo_() { thread_local uintptr_t v = 0; return v; }

inline void record_stack_bounds_() {
#ifdef __linux__
    pthread_attr_t a;
    if (::pthread_getattr_np(::pthread_self(), &a) == 0) {
        void* base = nullptr; size_t size = 0;
        if (::pthread_attr_getstack(&a, &base, &size) == 0)
            tls_stack_lo_() = reinterpret_cast<uintptr_t>(base);
        ::pthread_attr_destroy(&a);
    }
#endif
}

inline unsigned int posix_seh_code_(int sig, int si_code, void* addr) {
    switch (sig) {
        case SIGSEGV:
        case SIGBUS: {
            // The stack grows down and its guard page sits just below the recorded
            // low bound. A fault whose address is in the guard region (or just
            // inside the stack's low edge) is a STACK_OVERFLOW, not a wild pointer
            // — the distinction the Win32 SEH code carries and the fault-policy
            // classifier keys on. 1 MiB window below the low bound is generous
            // enough for a deep single frame that jumped the guard.
            uintptr_t lo = tls_stack_lo_();
            if (lo && addr) {
                uintptr_t fa = reinterpret_cast<uintptr_t>(addr);
                if (fa + (uintptr_t(1) << 20) >= lo && fa <= lo + 4096u)
                    return 0xC00000FD;   // STACK_OVERFLOW
            }
            return 0xC0000005;           // ACCESS_VIOLATION
        }
        case SIGILL:  return 0xC000001D; // ILLEGAL_INSTRUCTION
        case SIGFPE:
#ifdef FPE_INTDIV
            if (si_code == FPE_INTDIV) return 0xC0000094; // INT_DIVIDE_BY_ZERO
#endif
#ifdef FPE_FLTDIV
            if (si_code == FPE_FLTDIV) return 0xC0000091; // FLOAT_DIVIDE_BY_ZERO
#endif
            return 0xC0000090;           // FLOAT_INVALID_OPERATION (generic FP)
        default:      return 0;          // UNKNOWN_SEH_EXCEPTION
    }
}

// The address of the most recent hardware fault on THIS thread, stashed by the
// handler right before it throws (a single async-signal-safe pointer store). The
// crash-forensics layer (xi_crash_dump.hpp) reads it on the faulting thread when
// an uncaught seh_exception reaches std::terminate, to dladdr-blame the module —
// the throw has unwound the stack by then, so the fault site is otherwise lost.
inline void*& last_fault_addr() { thread_local void* a = nullptr; return a; }

inline void posix_seh_handler_(int sig, siginfo_t* info, void* /*ucontext*/) {
    last_fault_addr() = info ? info->si_addr : nullptr;
    throw seh_exception(posix_seh_code_(sig, info ? info->si_code : 0,
                                        info ? info->si_addr : nullptr));
}

// A STACK_OVERFLOW fault is a SIGSEGV delivered when the thread's stack is already
// exhausted — the handler can only run on a SEPARATE stack. sigaltstack is
// per-thread, so each thread that guards untrusted code registers its own alt
// stack (once) before installing the handlers. thread_local storage keeps it
// alive for the thread's lifetime and reclaims it automatically at thread exit.
inline void ensure_altstack_() {
    thread_local std::vector<char> alt_stack;
    thread_local bool installed = false;
    if (installed) return;
    // SIGSTKSZ can be a runtime value on modern glibc; a generous fixed floor is
    // fine and avoids depending on the exact macro form.
    const size_t sz = (size_t)SIGSTKSZ > 65536 ? (size_t)SIGSTKSZ : 65536;
    alt_stack.resize(sz);
    stack_t ss;
    std::memset(&ss, 0, sizeof(ss));
    ss.ss_sp    = alt_stack.data();
    ss.ss_size  = alt_stack.size();
    ss.ss_flags = 0;
    if (::sigaltstack(&ss, nullptr) == 0) installed = true;
}

inline void install_seh_translator() {
    ensure_altstack_();
    record_stack_bounds_();
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = posix_seh_handler_;
    // SA_ONSTACK: run the handler on the per-thread alt stack so a stack-overflow
    // SIGSEGV can still be caught. SA_NODEFER: leave the signal unblocked as we
    // exit the handler via a throw rather than a normal return.
    sa.sa_flags     = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGSEGV, &sa, nullptr);
    ::sigaction(SIGFPE,  &sa, nullptr);
    ::sigaction(SIGBUS,  &sa, nullptr);
    ::sigaction(SIGILL,  &sa, nullptr);
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

// Layering-safe pre-_Exit drain hook. recover_seh_stack_or_die may hard-exit while
// a SIBLING thread is mid-write_minidump; a bare _Exit truncates that dump + drops
// its .json sidecar. The drain (xi::crash::await_dump) that lets the sibling finish
// lives in xi_crash_dump.hpp — the HIGHER layer (it #includes THIS file), so this
// header cannot call it directly without a circular include. Instead the higher
// layer registers await_dump here as a void()-thunk at crash::install(); the helper
// below CALLS it (if non-null) right before _Exit. Null until registered (and
// off-Windows, where the guard page can't fail and no _Exit path is reached), so
// the default behavior is an immediate exit with no drain.
inline std::atomic<void(*)()>& seh_predump_drain_hook() {
    static std::atomic<void(*)()> hook{nullptr};
    return hook;
}

// recover_seh_stack(), plus the unrecoverable-guard-page policy: if the guard page
// cannot be restored the thread's stack is compromised and cannot be run again, so
// we hard-exit crash-safely for the FE supervisor to respawn — the same trade the
// watchdog HARD trip makes (drain an in-flight sibling minidump via the drain hook,
// log, then std::_Exit, skipping dtors a wedged thread could deadlock on). Used at
// every seh_exception catch site whose thread survives to run
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
    // Let a sibling thread's in-flight minidump land before we _Exit (H7). The
    // hook is xi::crash::await_dump when registered; null → immediate exit.
    if (auto drain = seh_predump_drain_hook().load(std::memory_order_acquire))
        drain();
    std::_Exit(kStackGuardExitCode);
}
inline void recover_seh_stack_or_die(const seh_exception& e, const char* where) {
    recover_seh_stack_or_die(e.code, where);
}

} // namespace xi
