#pragma once
//
// xi_crash_dump.hpp — minidump + crash-report forensics for the compute core.
//
// When the in-process backend dies on a fatal fault, a top-level handler writes
// a minidump + a sibling .json crash report under %TEMP%/xinsp2/crashdumps. The
// report names the faulting module (script vs plugin vs core) and the last
// activity breadcrumb per thread, so the next startup / the FE can surface
// *which* component caused the death (read back via cmd:crash_reports).
//
// Two halves:
//   * breadcrumb model (portable) — a per-thread Context the dispatch hot path
//     stamps with the current cmd/instance/plugin/phase. Read by the writer.
//   * dump machinery (Windows-only) — the unhandled-exception filter, the CRT
//     death-path interceptors (terminate/abort/invalid-param/purecall), the
//     vectored first-chance logger, and install().
//
// Extracted from service_main.cpp. The host keeps only thin forwarders at the
// hot-path breadcrumb sites + one install() call at boot.
//
// TODO(linux): the dump machinery is dbghelp/SEH-specific; the Linux port needs
// a signalfd/backtrace equivalent. The breadcrumb model is already portable.
//
#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <dbghelp.h>
  #include <psapi.h>
  #pragma comment(lib, "dbghelp.lib")  // top-level crash filter
  #pragma comment(lib, "psapi.lib")    // module-blame lookup
#else
  #include <execinfo.h>   // backtrace / backtrace_symbols_fd (stop-gap dump)
  #include <dlfcn.h>      // dladdr — module blame from an instruction address
  #include <unistd.h>     // getpid
  #include <ctime>        // timestamp for the report stem
  #include <fcntl.h>
#endif

#include "xi_seh.hpp"   // xi::seh_exception + xi::last_fault_addr()

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <typeinfo>

// Stamped into each report so a dump can be tied to the exact build. CMake
// passes these; fall back so a stray include still compiles.
#ifndef XINSP2_VERSION
#define XINSP2_VERSION "unknown"
#endif
#ifndef XINSP2_COMMIT
#define XINSP2_COMMIT "unknown"
#endif

namespace xi {
namespace crash {

// ---- breadcrumb model (portable) --------------------------------------------
//
// A snapshot of "what was happening" updated by the dispatch hot path. Read by
// the unhandled-exception filter to produce a human-readable report alongside
// the minidump. Pure POD + plain strncpy so the filter is signal-safe (no
// allocations, no locks).
struct Context {
    uint32_t thread_id     = 0;  // owning thread (0 = slot free)
    char last_cmd[64]      {};   // last cmd handled
    char last_script[260]  {};   // last loaded script DLL path
    char last_instance[64] {};   // last instance whose plugin we called
    char last_plugin[64]   {};   // plugin name backing it
    char last_phase[32]    {};   // inspect lifecycle phase (reset/inspect/...)
    char last_status[96]   {};   // last xi::status()/set_status text on this thread
    int64_t last_run_id    = 0;  // full run id (wire run_id is int64; don't truncate)
    int  last_frame        = 0;  // frame hint — int by the xi_inspect_entry(int) ABI
};

// Per-thread crash breadcrumbs. A single global was racy under dispatch_threads
// > 1 — N concurrent inspects all wrote the same struct, so a crash dump could
// blame the wrong thread's plugin. Each thread claims a fixed slot (keyed by
// thread id) on first use and RELEASES it when the owning thread exits, so the
// table tracks LIVE threads, not lifetime threads. The crash handler walks all
// claimed slots and flags the one matching the faulting thread.
//
// Reclaim matters because the dispatch model produces a steady stream of distinct
// thread ids: every non-continuous frame runs on a fresh detached thread, and
// stop/reload joins + respawns the lane workers. Without reclaim the 64 slots
// filled monotonically and, after 64 lifetime threads, every later inspect fell
// through to a racy shared slot 0 — collapsing attribution and listing 64 dead
// threads in the report as if live. The reclaim uses the same thread_local-dtor
// pattern DocChunkPool::Heads uses to return thread-local pool blocks on exit.
inline constexpr int kMaxSlots = 64;
inline Context              g_slots[kMaxSlots];
inline std::atomic<uint32_t> g_slot_tid[kMaxSlots];

namespace detail {
// Owns one thread's breadcrumb slot for the thread's lifetime. The destructor
// (thread exit) clears the breadcrumb and frees the slot for reuse, so a
// long-running deployment can't exhaust the 64 slots. Clearing the Context BEFORE
// releasing the tid means a thread that later reclaims this slot never inherits a
// dead thread's breadcrumb, and the crash-report walk (keyed on a non-zero tid)
// never lists a reclaimed slot.
struct SlotGuard {
    int idx = -1;
    ~SlotGuard() {
        if (idx < 0) return;
        g_slots[idx] = Context{};                                   // clear first
        g_slot_tid[idx].store(0, std::memory_order_release);        // then release
    }
};
} // namespace detail

inline Context& ctx() {
    static thread_local detail::SlotGuard guard;
    if (guard.idx >= 0) return g_slots[guard.idx];
#ifdef _WIN32
    uint32_t tid = (uint32_t)GetCurrentThreadId();
#elif defined(__linux__)
    // Kernel thread id — small, stable, always non-zero (0 means "slot free"), so
    // each live thread claims a distinct breadcrumb slot and the crash report's
    // threads[] can tell them apart (per-thread plugin attribution).
    uint32_t tid = (uint32_t)::gettid();
#else
    // Portable fallback (e.g. macOS): fold pthread_self() and force non-zero.
    uint32_t tid = (uint32_t)(uintptr_t)::pthread_self();
    if (tid == 0) tid = 1;
#endif
    for (int i = 0; i < kMaxSlots; ++i) {
        uint32_t expected = 0;
        if (g_slot_tid[i].compare_exchange_strong(
                expected, tid, std::memory_order_acq_rel)) {
            guard.idx = i;
            g_slots[i].thread_id = tid;
            return g_slots[i];
        }
    }
    // Slots exhausted (>64 CONCURRENTLY-live threads) — fall back to slot 0. Racy
    // but never null; with per-thread reclaim this needs 64 threads alive AT ONCE,
    // not merely 64 over the process lifetime.
    return g_slots[0];
}

inline void set(char* dst, size_t n, const char* src) {
    if (!dst || !src) return;
    std::strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

// Round-3 #8: death-path breadcrumb stamp. on_terminate / raise_for_dump used
// to call ctx() to stamp last_cmd — but ctx() can CLAIM a fresh slot, and
// constructing its thread_local SlotGuard may allocate inside the CRT's
// TLS-dtor registration. That violates the filter's own rule (see the
// "looked up READ-ONLY by tid, not via ctx()" comment in write_minidump): on a
// heap-corruption abort the allocation can fault inside broken malloc and lose
// the dump entirely. This helper uses the same read-only scan-by-tid the
// filter uses — stamp only if this thread ALREADY owns a slot, never claim on
// the death path (a thread with no slot simply reports empty fields, exactly
// what a freshly claimed slot would have shown anyway).
inline void stamp_death_breadcrumb(const char* cause) noexcept {
#ifdef _WIN32
    uint32_t tid = (uint32_t)GetCurrentThreadId();
#else
    uint32_t tid = 1;  // TODO(linux): pthread_self()-derived id (match ctx())
#endif
    for (int i = 0; i < kMaxSlots; ++i) {
        if (g_slot_tid[i].load(std::memory_order_acquire) == tid) {
            set(g_slots[i].last_cmd, sizeof(g_slots[i].last_cmd), cause);
            return;
        }
    }
}

// Convenience for setting the current thread's inspect phase.
inline void set_phase(const char* phase) {
    auto& c = ctx();
    set(c.last_phase, sizeof(c.last_phase), phase);
}

// ---- process-global "current culprit" (Part III G2.1) -----------------------
//
// The per-thread breadcrumb above pinpoints the faulting THREAD; this is a
// single process-wide stamp of the plugin/instance the host most recently
// ENTERED (create / process / exchange), mirroring Part I A1's g_inspect_tid
// (a non-thread-local atomic/struct). Two reasons it is process-global and
// separate from the per-thread slot:
//   1. it also carries the plugin's FOLDER + DLL, so the FE can quarantine the
//      offender (write G1's .xi_certify.json) without a plugin registry of its
//      own — the per-thread breadcrumb only has the plugin name;
//   2. it is a fallback when the fault lands on an UNMANAGED thread the plugin
//      spawned itself (no breadcrumb slot of its own).
// Plain char buffers + plain set() like Context — signal-safe (no alloc/lock),
// bounded-racy under concurrent dispatch. The FE cross-checks the stamped DLL
// against the faulting module before attributing, so a stale/racing stamp can
// only FAIL to attribute, never MIS-attribute a script/core crash to a plugin.
struct Culprit {
    char instance[64] {};
    char plugin[64]   {};
    char folder[260]  {};
    char dll[96]      {};
};
inline Culprit g_culprit;

// Stamp the current culprit. null args clear the corresponding field. Called at
// the host->plugin call boundary (service_main.cpp). Cheap: four strncpy.
inline void set_culprit(const char* instance, const char* plugin,
                        const char* folder, const char* dll) {
    set(g_culprit.instance, sizeof(g_culprit.instance), instance ? instance : "");
    set(g_culprit.plugin,   sizeof(g_culprit.plugin),   plugin   ? plugin   : "");
    set(g_culprit.folder,   sizeof(g_culprit.folder),   folder   ? folder   : "");
    set(g_culprit.dll,      sizeof(g_culprit.dll),      dll      ? dll      : "");
}

// ---- dump-in-progress flag (H3 gate + H7 hard-exit rendezvous) ---------------
//
// Set by write_minidump while it is inside dbghelp; cleared when the dump + json
// sidecar are on disk. Two consumers of the one shared flag:
//   H3 — write_minidump serializes on it. dbghelp/MiniDumpWriteDump is NOT
//        thread-safe and the OS does NOT suspend sibling threads during an
//        unhandled-exception filter, so two+ dying threads (multiplied by the
//        terminate/abort re-raise paths) can enter the filter at once. The FIRST
//        faulter wins the flag and writes; a later entrant is dying anyway and
//        must NOT re-enter dbghelp (concurrent calls corrupt or HANG the dump —
//        and a hang means the process never self-exits).
//   H7 — the watchdog / drain / overflow hard-exit paths call await_dump() before
//        std::_Exit so an in-flight dump can finish rather than be truncated. The
//        STACK_OVERFLOW helper (xi::recover_seh_stack_or_die, xi_seh.hpp — a LOWER
//        layer that can't include this one) reaches await_dump indirectly: install()
//        registers it into xi::seh_predump_drain_hook() as a void()-thunk.
// Portable atomic so the helpers compile off-Windows too (where the flag never
// sets and await_dump() is an immediate no-op).
inline std::atomic<long> g_dump_in_progress{0};

// True while a minidump is actively being written by some thread.
inline bool dump_in_progress() {
    return g_dump_in_progress.load(std::memory_order_acquire) != 0;
}

// Best-effort: spin (yielding) up to timeout_ms while a dump is in progress, so
// an imminent hard exit lets an in-flight dump land first. Bounded — a dump that
// overruns the timeout still yields to the caller's _Exit; never blocks shutdown
// forever. Off-Windows the flag never sets, so this returns immediately.
inline void await_dump(unsigned timeout_ms) {
    unsigned waited = 0;
    while (dump_in_progress() && waited < timeout_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        waited += 5;
    }
}

#ifdef _WIN32
// ---- dump machinery (Windows-only) ------------------------------------------

// Dump directory (e.g. %TEMP%\xinsp2\crashdumps), resolved + created ONCE at
// install() time — on a healthy heap — into a fixed static buffer. The fatal
// filter reads it without touching std::filesystem or the heap. Empty means
// install() hasn't run (or resolution failed); the filter then falls back to a
// Win32-only (still alloc-free) GetTempPathA + CreateDirectoryA resolution.
inline char g_dump_dir[MAX_PATH] = {};

// Map an instruction pointer to "<module>+0x<offset>" by scanning loaded
// modules, writing into a caller-provided fixed buffer. Used in the crash
// filter to point at which DLL (script vs plugin vs xinsp-backend itself) was
// executing. Alloc-free (stack buffers + snprintf + Win32 only): it runs on
// faults whose root cause may be the heap itself (0xC0000374), so it must
// honor the same signal-safe discipline as the POD breadcrumb model.
inline void blame_module_into(void* addr, char* out, size_t out_sz) {
    std::snprintf(out, out_sz, "<unknown>");
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return;
    int n = (int)(needed / sizeof(HMODULE));
    for (int i = 0; i < n; ++i) {
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        auto base = (uintptr_t)mi.lpBaseOfDll;
        if ((uintptr_t)addr < base || (uintptr_t)addr >= base + mi.SizeOfImage) continue;
        char name[MAX_PATH];
        GetModuleFileNameA(mods[i], name, sizeof(name));
        const char* slash = std::strrchr(name, '\\');
        std::snprintf(out, out_sz, "%s+0x%llx", slash ? slash + 1 : name,
                      (unsigned long long)((uintptr_t)addr - base));
        return;
    }
}

namespace detail {
// Bounded appender for the fatal path: writes into a caller-provided fixed
// buffer, silently truncating when full (never overruns, always leaves room
// for a NUL). No heap, no locks — the crash filter's signal-safe discipline.
// Truncation over allocation: a clipped sidecar field beats a filter that
// faults in operator new on the corrupt heap it is reporting on.
struct FixedWriter {
    char*  buf;
    size_t cap;      // total capacity, including the trailing NUL
    size_t len = 0;
    void put(char c) {
        if (len + 1 < cap) buf[len++] = c;
    }
    void append(const char* s) {
        while (*s) put(*s++);
    }
    void appendf(const char* fmt, ...) {
        if (len + 1 >= cap) return;
        va_list ap;
        va_start(ap, fmt);
        int n = std::vsnprintf(buf + len, cap - len, fmt, ap);
        va_end(ap);
        if (n <= 0) return;
        size_t room = cap - len - 1;                 // chars actually writable
        len += ((size_t)n < room) ? (size_t)n : room;
    }
    void finish() { buf[len < cap ? len : cap - 1] = 0; }
};

// JSON-escape s (quoted) into w. Alloc-free replacement for the old
// std::string json_escape; tiny copy of xp::json_escape_into's escape set to
// keep this filter free of any nontrivial dep.
inline void json_escape_append(FixedWriter& w, const char* s) {
    w.put('"');
    for (; *s; ++s) {
        char c = *s;
        switch (c) {
            case '"':  w.append("\\\""); break;
            case '\\': w.append("\\\\"); break;
            case '\n': w.append("\\n");  break;
            case '\r': w.append("\\r");  break;
            case '\t': w.append("\\t");  break;
            default:   w.put(c);
        }
    }
    w.put('"');
}
} // namespace detail

inline const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:        return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:          return "STACK_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:      return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:      return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:   return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_ILLEGAL_INSTRUCTION:     return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:        return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:           return "IN_PAGE_ERROR";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:return "NONCONTINUABLE";
        case 0xE06D7363:                        return "MS_C++_EXCEPTION";
        // Synthetic codes we RaiseException with so write_minidump runs for
        // CRT death paths that bypass SEH (terminate/abort/fastfail family).
        case 0xE0000001:                        return "TEST_CRASH";
        case 0xE0000002:                        return "CXX_TERMINATE";
        case 0xE0000003:                        return "CXX_ABORT";
        case 0xE0000004:                        return "CRT_INVALID_PARAMETER";
        case 0xE0000005:                        return "CXX_PURE_CALL";
        default:                                return "UNKNOWN";
    }
}

// Reserve stack headroom so the unhandled-exception filter (write_minidump) can
// still run after a STACK_OVERFLOW — otherwise the filter has no stack left and
// the process dies with NO minidump/sidecar (robustness BUG 2). Call once at the
// top of every thread that runs untrusted inspect/plugin code.
inline void reserve_fault_stack() {
    ULONG guarantee = 128 * 1024;  // 128 KB — room for the filter + MiniDumpWriteDump
    SetThreadStackGuarantee(&guarantee);
}

// Top-level unhandled-exception filter. Writes a minidump under
// %TEMP%/xinsp2/crashdumps PLUS a sibling .json crash report containing
// exception kind, faulting module, and the last activity context. The report is
// read by the backend on the NEXT startup and surfaced via cmd:crash_reports —
// the extension shows it as a notification so the user knows *which* component
// (script / plugin / core) caused the last session's death.
//
// INVARIANT: this filter is signal-safe — no heap, no locks. It must keep
// working when the fault IS the heap (0xC0000374 heap corruption), so the
// whole path uses only stack/static buffers, snprintf, and raw Win32 file
// APIs. The dump directory is pre-resolved at install(); anything heap-y
// (std::string, std::filesystem, new/malloc) is banned here.
inline LONG WINAPI write_minidump(EXCEPTION_POINTERS* info) {
    // H3 — serialize against concurrent unhandled faults. The FIRST faulter wins
    // the flag and writes the dump. A later entrant is dying regardless; it must
    // NOT touch dbghelp (concurrent MiniDumpWriteDump corrupts or HANGS) and must
    // NOT return EXCEPTION_CONTINUE_SEARCH (that would fall into a second dbghelp
    // call up the chain). It spins briefly — giving the real dump time to land —
    // then EXECUTE_HANDLERs so the process terminates.
    long expected = 0;
    if (!g_dump_in_progress.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel)) {
        for (int i = 0; i < 200 && dump_in_progress(); ++i)
            Sleep(10);   // ~2s cap; the owner is writing our shared post-mortem
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // SIGNAL-SAFE WRITER INVARIANT — from here to the final return the filter
    // now honors the discipline the breadcrumb model documents: NO heap, NO
    // locks. Only stack/static buffers, snprintf, and raw Win32 file APIs
    // (CreateFileA/WriteFile). No std::string, no std::filesystem, no
    // new/malloc. This matters because the product's most serious crash class
    // is heap corruption (0xC0000374, flagged by veh_logger below): on that
    // fault the heap is the thing that just failed, and a single std::string
    // append here could fault or deadlock — killing the process with a
    // truncated/absent dump and no .json sidecar, i.e. forensics failing on
    // exactly the crash class they exist for.

    // Dump directory: pre-resolved + created at install() into g_dump_dir.
    // Fallback (filter fired before/without install, or resolution failed
    // there): GetTempPathA + CreateDirectoryA — still alloc-free.
    char dir[MAX_PATH];
    if (g_dump_dir[0]) {
        std::snprintf(dir, sizeof(dir), "%s", g_dump_dir);
    } else {
        char tmp[MAX_PATH];
        DWORD tn = GetTempPathA((DWORD)sizeof(tmp), tmp);  // ends with '\'
        if (tn == 0 || tn >= sizeof(tmp)) { tmp[0] = '.'; tmp[1] = '\\'; tmp[2] = 0; }
        std::snprintf(dir, sizeof(dir), "%sxinsp2", tmp);
        CreateDirectoryA(dir, nullptr);                    // ok if it exists
        std::snprintf(dir, sizeof(dir), "%sxinsp2\\crashdumps", tmp);
        CreateDirectoryA(dir, nullptr);
    }

    SYSTEMTIME st; GetLocalTime(&st);
    // Stem carries tid + milliseconds (on top of pid + HH:MM:SS) so two faults in
    // the same wall-clock second on different threads never collide on the dump
    // filename (CreateFileA below opens with dwShareMode=0).
    char stem[160];
    std::snprintf(stem, sizeof(stem),
        "xinsp-backend-%lu-%04d%02d%02d-%02d%02d%02d%03d-t%lu",
        (unsigned long)GetCurrentProcessId(),
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        st.wMilliseconds, (unsigned long)GetCurrentThreadId());
    char dmp_path[MAX_PATH + 176];
    char json_path[MAX_PATH + 176];
    std::snprintf(dmp_path,  sizeof(dmp_path),  "%s\\%s.dmp",  dir, stem);
    std::snprintf(json_path, sizeof(json_path), "%s\\%s.json", dir, stem);

    DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    void* addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
    char blamed[MAX_PATH + 32];
    blame_module_into(addr, blamed, sizeof(blamed));

    // 1. Minidump
    HANDLE h = CreateFileA(dmp_path, GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers    = FALSE;
        // Richer than MiniDumpNormal so the dump is self-contained for
        // post-mortem of an in-process compute-core crash:
        //   WithDataSegs              — globals (breadcrumb table,
        //                               recent_errors ring) land in the dump
        //   WithThreadInfo            — per-thread times / teb
        //   WithIndirectlyReferenced  — pointee memory of stack locals
        //                               (e.g. the TriggerEvent being inspected)
        //   WithUnloadedModules       — a just-FreeLibrary'd plugin still
        //                               shows in the module list for blame
        // Deliberately NOT WithFullMemory — large image buffers would
        // bloat the dump to GBs; the above captures the forensic state
        // without the bulk.
        auto dump_type = (MINIDUMP_TYPE)(
            MiniDumpNormal
            | MiniDumpWithDataSegs
            | MiniDumpWithThreadInfo
            | MiniDumpWithIndirectlyReferencedMemory
            | MiniDumpWithUnloadedModules);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                          dump_type, &mei, nullptr, nullptr);
        CloseHandle(h);
    }

    // 2. JSON sidecar — what the next-startup report path reads. Built with
    // the bounded FixedWriter (snprintf-style appends, truncate-don't-allocate)
    // into a STATIC buffer, not the stack: the filter may be running on the
    // 128 KB SetThreadStackGuarantee fault stack after a STACK_OVERFLOW, and
    // the worst-case sidecar (64 live thread slots, every field full +
    // escaped) is ~56 KB. Sharing the static buffer is safe: the H3 flag above
    // guarantees exactly one thread is in this section at a time.
    static char g_json_buf[64 * 1024];
    detail::FixedWriter w{ g_json_buf, sizeof(g_json_buf) };
    w.append("{\"version\":\"" XINSP2_VERSION "\""
             ",\"commit\":\"" XINSP2_COMMIT "\"");
    w.appendf(",\"pid\":%lu", (unsigned long)GetCurrentProcessId());
    w.appendf(",\"thread_id\":%lu", (unsigned long)GetCurrentThreadId());
    char tsbuf[64];
    std::snprintf(tsbuf, sizeof(tsbuf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    w.append(",\"timestamp\":");
    detail::json_escape_append(w, tsbuf);
    w.appendf(",\"exception\":{\"code\":\"0x%08X\"", (unsigned)code);
    w.append(",\"name\":");
    detail::json_escape_append(w, exception_name(code));
    w.appendf(",\"address\":\"0x%llx\"", (unsigned long long)(uintptr_t)addr);
    w.append(",\"module\":");
    detail::json_escape_append(w, blamed);
    w.append("}");
    // `context` = the faulting thread's breadcrumb (the handler runs on the
    // faulting thread, so its slot — looked up READ-ONLY by tid, not via
    // ctx(): ctx() can CLAIM a slot, and constructing its thread_local
    // SlotGuard on this path may allocate inside the CRT's TLS-dtor
    // registration. A thread with no slot yields empty fields, exactly what a
    // freshly claimed slot would have shown. Back-compat with the existing
    // report reader which expects this object.
    uint32_t fault_tid = (uint32_t)GetCurrentThreadId();
    {
        Context c_none{};                 // stack fallback: no slot => empty fields
        const Context* cp = &c_none;
        for (int i = 0; i < kMaxSlots; ++i) {
            if (g_slot_tid[i].load(std::memory_order_acquire) == fault_tid) {
                cp = &g_slots[i];
                break;
            }
        }
        const Context& c = *cp;
        w.append(",\"context\":{");
        w.append("\"last_cmd\":");      detail::json_escape_append(w, c.last_cmd);
        w.append(",\"last_script\":");  detail::json_escape_append(w, c.last_script);
        w.append(",\"last_instance\":");detail::json_escape_append(w, c.last_instance);
        w.append(",\"last_plugin\":");  detail::json_escape_append(w, c.last_plugin);
        w.append(",\"last_phase\":");   detail::json_escape_append(w, c.last_phase);
        w.append(",\"last_status\":");  detail::json_escape_append(w, c.last_status);
        w.appendf(",\"last_run_id\":%lld", (long long)c.last_run_id);
        w.appendf(",\"last_frame\":%d", c.last_frame);
        w.append("}");
    }
    // `culprit` (Part III G2.1) = the process-global plugin/instance the host
    // last entered, plus that plugin's folder + dll. The FE reads this to
    // attribute the death to a plugin and (cross-checked against `module`)
    // quarantine it via G1's .xi_certify.json. Empty fields = not attributable.
    {
        w.append(",\"culprit\":{");
        w.append("\"instance\":");  detail::json_escape_append(w, g_culprit.instance);
        w.append(",\"plugin\":");   detail::json_escape_append(w, g_culprit.plugin);
        w.append(",\"folder\":");   detail::json_escape_append(w, g_culprit.folder);
        w.append(",\"dll\":");      detail::json_escape_append(w, g_culprit.dll);
        w.append("}");
    }
    // `threads` = every claimed breadcrumb slot, so a multi-dispatch crash shows
    // what ALL concurrent inspects were doing, not just the faulting one.
    // `faulting:true` flags the culprit.
    w.append(",\"threads\":[");
    {
        bool first = true;
        for (int i = 0; i < kMaxSlots; ++i) {
            uint32_t tid = g_slot_tid[i].load(std::memory_order_acquire);
            if (tid == 0) continue;
            auto& c = g_slots[i];
            if (!first) w.append(",");
            first = false;
            w.appendf("{\"thread_id\":%lu", (unsigned long)tid);
            w.append(tid == fault_tid ? ",\"faulting\":true" : ",\"faulting\":false");
            w.append(",\"last_cmd\":");     detail::json_escape_append(w, c.last_cmd);
            w.append(",\"last_instance\":");detail::json_escape_append(w, c.last_instance);
            w.append(",\"last_plugin\":");  detail::json_escape_append(w, c.last_plugin);
            w.append(",\"last_phase\":");   detail::json_escape_append(w, c.last_phase);
            w.append(",\"last_status\":");  detail::json_escape_append(w, c.last_status);
            w.appendf(",\"last_run_id\":%lld", (long long)c.last_run_id);
            w.appendf(",\"last_frame\":%d", c.last_frame);
            w.append("}");
        }
    }
    w.append("]");
    w.append(",\"minidump\":");
    char dmpname[176];
    std::snprintf(dmpname, sizeof(dmpname), "%s.dmp", stem);
    detail::json_escape_append(w, dmpname);
    w.append("}\n");
    w.finish();

    HANDLE jh = CreateFileA(json_path, GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (jh != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(jh, w.buf, (DWORD)w.len, &wrote, nullptr);
        CloseHandle(jh);
    }

    // Dump + sidecar are on disk. Release the H3 flag FIRST — before the stdio
    // logging below (fprintf takes the CRT stream lock, the one step here that
    // can still block) — so a hard-exit path blocked in await_dump() (H7)
    // resumes promptly instead of waiting out its full bound.
    g_dump_in_progress.store(0, std::memory_order_release);

    std::fprintf(stderr, "[xinsp2] CRASH 0x%08X (%s) in %s — minidump: %s\n",
                 code, exception_name(code), blamed, dmp_path);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

// std::terminate handler — fires when an unhandled C++ exception unwinds out of
// a thread (e.g. a detached worker thread that didn't wrap its body in
// try/catch). This path bypasses SetUnhandledExceptionFilter on its own, so a
// silent terminate would produce no crashdump. We:
//   1. Log the current exception's what()/type so the cause appears in stderr.
//   2. RaiseException with a recognisable code so write_minidump sees a thread
//      context and can write the dump + json sidecar.
[[noreturn]] inline void on_terminate() noexcept {
    const char* what  = "<no exception>";
    const char* tname = "<no exception>";
    try {
        if (auto p = std::current_exception()) std::rethrow_exception(p);
    } catch (const std::exception& e) {
        what  = e.what();
        tname = typeid(e).name();
    } catch (const xi::seh_exception& e) {
        what  = e.what();
        tname = "xi::seh_exception";
    } catch (...) {
        tname = "<non-std exception>";
    }
    std::fprintf(stderr,
        "[xinsp2] std::terminate (thread %lu): %s — %s\n",
        (unsigned long)GetCurrentThreadId(), tname, what);
    std::fflush(stderr);
    // Round-3 #8: read-only scan-by-tid — never ctx() (may claim/allocate) on
    // the death path; see stamp_death_breadcrumb.
    stamp_death_breadcrumb("terminate");
    // 0xE0000002 — distinct from --test-crash's 0xE0000001 so blame_module and
    // exception_name still tag it as MS_C++ish; the json_path will record this
    // code so the next-startup report distinguishes the two paths. NONCONTINUABLE
    // so the filter actually runs.
    RaiseException(0xE0000002, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::abort();   // unreachable; quiets [[noreturn]]
}

// CRT abort()/fastfail family — std::abort(), a failed C `assert`, a CRT
// invalid-parameter trip, or a pure-virtual call all terminate the process via
// __fastfail (0xC0000409), which bypasses BOTH SetUnhandledExceptionFilter
// (write_minidump) AND std::set_terminate (on_terminate). Without a handler a
// script that calls abort() kills the backend leaving NO minidump / .json
// sidecar — defeating cmd:crash_reports AND the FE crash-history / status
// channel (their forensics come from that sidecar). We intercept each entry
// point and re-raise a NONCONTINUABLE exception so write_minidump runs with a
// real thread context (same trick as on_terminate). Robustness BUG 1, found by
// the robustness-fuzzer dogfood; see docs/internals/fe-be.md crash story.
[[noreturn]] inline void raise_for_dump(const char* cause, DWORD code) noexcept {
    // Round-3 #8: read-only scan-by-tid — never ctx() (may claim/allocate) on
    // the death path; a heap-corruption abort would fault inside broken malloc
    // and lose the dump. See stamp_death_breadcrumb.
    stamp_death_breadcrumb(cause);
    std::fprintf(stderr, "[xinsp2] CRT fatal (%s) — writing crash report\n", cause);
    std::fflush(stderr);
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::abort();   // unreachable; quiets [[noreturn]]
}
inline void on_sigabrt(int) { raise_for_dump("abort", 0xE0000003); }
inline void on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*,
                                 unsigned int, uintptr_t) {
    raise_for_dump("invalid_parameter", 0xE0000004);
}
inline void on_purecall() { raise_for_dump("purecall", 0xE0000005); }

// Vectored exception handler — runs BEFORE SEH translators, before any
// per-thread try/__except. Logs first-chance exceptions that might get swallowed
// silently. Returning EXCEPTION_CONTINUE_SEARCH lets normal handling proceed;
// we're just listening here.
//
// Filtered to the codes that would actually kill the process if unhandled: AVs,
// illegal instructions, stack overflow, fastfail, our own RaiseException codes.
// Skipping benign first-chance C++ exceptions (0xE06D7363) that happen all the
// time during normal try/catch flow.
inline LONG WINAPI veh_logger(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = info->ExceptionRecord->ExceptionCode;
    bool concerning =
        code == EXCEPTION_ACCESS_VIOLATION ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_NONCONTINUABLE_EXCEPTION ||
        code == 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN / fastfail */ ||
        code == 0xC0000374 /* STATUS_HEAP_CORRUPTION */ ||
        (code >= 0xE0000001 && code <= 0xE0000010);
    if (concerning) {
        void* addr = info->ExceptionRecord->ExceptionAddress;
        // Alloc-free blame: this logger fires FIRST-CHANCE on 0xC0000374
        // (heap corruption) — allocating a std::string here would run on the
        // very heap that just failed, before the real filter even gets a shot.
        char blamed[MAX_PATH + 32];
        blame_module_into(addr, blamed, sizeof(blamed));
        std::fprintf(stderr,
            "[xinsp2] VEH first-chance 0x%08X (%s) thread %lu at %s\n",
            code, exception_name(code),
            (unsigned long)GetCurrentThreadId(), blamed);
        std::fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Install every crash-forensics handler. Call once at process start (the SEH
// translator _set_se_translator is a separate concern owned by the host). The
// CRT fastfail family (abort / failed assert / invalid-parameter / pure call)
// bypasses SetUnhandledExceptionFilter / set_terminate, so each entry point gets
// its own interceptor — a crash report is ALWAYS written.
inline void install() {
    // Resolve + create the dump directory ONCE, here, on a healthy heap.
    // std::filesystem (and any other allocation) is banned inside the fatal
    // filter — its most serious customer is heap corruption (0xC0000374) —
    // so the heap-y work happens at install time and the result lands in the
    // fixed static g_dump_dir the filter reads allocation-free. On any
    // failure g_dump_dir stays empty and write_minidump falls back to its
    // Win32-only GetTempPathA resolution.
    try {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dir = fs::temp_directory_path(ec);
        if (!ec) {
            dir /= "xinsp2";
            dir /= "crashdumps";
            fs::create_directories(dir, ec);
            if (!ec) {
                std::string s = dir.string();
                if (s.size() < sizeof(g_dump_dir))
                    std::snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", s.c_str());
            }
        }
    } catch (...) {
        g_dump_dir[0] = 0;   // fall back to the filter's Win32-only path
    }
    // Top-level guard: minidump on crashes that escape the SEH translator
    // (stack overflow, plugin static destructor faults, etc.).
    SetUnhandledExceptionFilter(write_minidump);
    // C++ terminate path — covers unhandled exceptions in detached threads.
    std::set_terminate(on_terminate);
    // Vectored handler — first crack at every concerning exception, even ones
    // that get suppressed downstream. Diagnostic only.
    AddVectoredExceptionHandler(/*first=*/1, veh_logger);
    // SIGABRT must have a handler installed BEFORE any abort(); _set_abort_behavior
    // clears the popup + the Watson/fastfail report so our handler is the path
    // that runs.
    std::signal(SIGABRT, on_sigabrt);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(on_invalid_parameter);
    _set_purecall_handler(on_purecall);
    reserve_fault_stack();   // BUG 2: dump on a main-thread stack overflow
    // H7 (layering-safe): xi::recover_seh_stack_or_die (xi_seh.hpp, the LOWER layer)
    // hard-exits on an unrecoverable STACK_OVERFLOW guard page. Give it await_dump so
    // a sibling thread's in-flight minidump lands before that _Exit instead of being
    // truncated. It can't call await_dump directly (circular include), so we inject
    // it here — the one-time crash init — as a void()-thunk into the drain hook.
    seh_predump_drain_hook().store(+[]{ await_dump(10000); },
                                   std::memory_order_release);
}

// Windows counterpart of the POSIX install_minidump_writer(): a NO-OP. Windows
// already writes real minidumps through its own SetUnhandledExceptionFilter path
// (write_minidump above) — Breakpad is the POSIX-only stop-gap. This exists so
// entrypoints (service_main / runner) can call it UNCONDITIONALLY right before
// install(), exactly as they do on POSIX; without it the Windows backend build
// fails with "'install_minidump_writer': is not a member of 'xi::crash'".
inline void install_minidump_writer() {}

#else  // !_WIN32
// ---- POSIX crash forensics (stop-gap) ---------------------------------------
//
// No minidump; instead, on an uncaught crash we write the SAME .json crash-report
// the FE's parser reads (exception.{name,module}, context.last_phase, culprit,
// threads[]) next to a <stem>.dmp text file carrying a symbolized backtrace for
// preservation, and print the "... — minidump: <stem>.dmp" trigger line the FE
// greps. This composes with the xi_seh.hpp fault→exception translator rather than
// fighting it for SIGSEGV: we hook std::set_terminate (a hardware fault becomes a
// thrown seh_exception; if it escapes unhandled it reaches terminate here) plus
// SIGABRT (the abort()/assert path the translator does not touch). Module blame
// uses xi::last_fault_addr() (dladdr) for the fault→throw path, else the top
// non-runtime backtrace frame. The FULL forensic dump (Breakpad/Crashpad) is the
// follow-up; this gives the FE non-empty forensics + a stack today.

inline void json_escape(std::string& out, const char* s) {
    out += '"';
    for (const char* p = s ? s : ""; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += (char)c;
        }
    }
    out += '"';
}

inline const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "ACCESS_VIOLATION";
        case SIGBUS:  return "BUS_ERROR";
        case SIGFPE:  return "FLOAT_EXCEPTION";
        case SIGILL:  return "ILLEGAL_INSTRUCTION";
        case SIGABRT: return "ABORT";
        default:      return "SIGNAL";
    }
}

// "<soname>+0x<off>" for an instruction address via dladdr, or "<unknown>".
inline std::string blame_addr(void* addr) {
    if (!addr) return "<unknown>";
    Dl_info info;
    if (dladdr(addr, &info) && info.dli_fname) {
        std::string base = std::filesystem::path(info.dli_fname).filename().string();
        uintptr_t off = (uintptr_t)addr - (uintptr_t)info.dli_fbase;
        char b[32]; std::snprintf(b, sizeof b, "+0x%lx", (unsigned long)off);
        return base + b;
    }
    return "<unknown>";
}

// First backtrace frame that is NOT in the C/C++ runtime, ld, or this exe — i.e.
// the most likely script/plugin culprit module. Falls back to "<unknown>".
inline std::string blame_backtrace(void* const* frames, int n) {
    for (int i = 0; i < n; ++i) {
        Dl_info info;
        if (!dladdr(frames[i], &info) || !info.dli_fname) continue;
        std::string base = std::filesystem::path(info.dli_fname).filename().string();
        if (base.rfind("libc", 0) == 0 || base.rfind("libstdc++", 0) == 0 ||
            base.rfind("libgcc", 0) == 0 || base.rfind("ld-", 0) == 0 ||
            base.rfind("libpthread", 0) == 0 || base.rfind("libm", 0) == 0)
            continue;
        uintptr_t off = (uintptr_t)frames[i] - (uintptr_t)info.dli_fbase;
        char b[32]; std::snprintf(b, sizeof b, "+0x%lx", (unsigned long)off);
        return base + b;
    }
    return "<unknown>";
}

inline std::string crash_dir_() {
    const char* t = std::getenv("TMPDIR");
    std::filesystem::path d = (t && *t) ? t : "/tmp";
    d /= "xinsp2"; d /= "crashdumps";
    std::error_code ec; std::filesystem::create_directories(d, ec);
    return d.string();
}

// Write the .json sidecar + a .dmp backtrace text, print the FE trigger line.
// `exc_name` is the exception/signal name; `fault_addr` the faulting instruction
// (may be null); frames/n a captured backtrace. First faulter wins (H3 analogue).
// Optional real-minidump writer, registered by the backend/runner when built with
// Breakpad (-DXINSP2_HAS_BREAKPAD). It writes a minidump into `dir`, fills `out`
// with the resulting .dmp path, and returns true on success. Kept as a function
// pointer so Breakpad stays isolated in ONE TU (backend/src/crash_breakpad.cpp)
// linked only into the backend + runner — this header, and the many test TUs that
// include it, never depend on Breakpad. Null → the text-backtrace fallback below.
using MinidumpWriterFn = bool (*)(const char* dir, char* out, int out_cap);
inline std::atomic<MinidumpWriterFn>& minidump_writer_hook() {
    static std::atomic<MinidumpWriterFn> h{nullptr};
    return h;
}
inline void set_minidump_writer(MinidumpWriterFn fn) {
    minidump_writer_hook().store(fn, std::memory_order_release);
}

#if defined(XINSP2_HAS_BREAKPAD)
// Defined in backend/src/crash_breakpad.cpp (the single Breakpad TU). Registers
// the Breakpad minidump writer into the hook above. Call once at boot, before
// install(). Only visible to TUs built with XINSP2_HAS_BREAKPAD (backend/runner).
void register_breakpad_writer();
#endif

// Wire up the best available minidump writer for this build: Breakpad when
// compiled in, otherwise nothing (the handler falls back to a text backtrace).
// No-op + zero dependency when XINSP2_HAS_BREAKPAD is off, so entrypoints can
// call it unconditionally right before install().
inline void install_minidump_writer() {
#if defined(XINSP2_HAS_BREAKPAD)
    register_breakpad_writer();
#endif
}

inline void write_crash_report_posix(const char* exc_name, void* fault_addr,
                                     void* const* frames, int nframes) {
    static std::atomic<int> once{0};
    if (once.fetch_add(1, std::memory_order_acq_rel) != 0) return;
    g_dump_in_progress.store(1, std::memory_order_release);

    std::string dir = crash_dir_();
    long pid = (long)getpid();
    std::time_t now = std::time(nullptr);
    std::tm tmv{}; gmtime_r(&now, &tmv);
    char stem[96];
    std::snprintf(stem, sizeof stem, "xinsp-backend-%ld-%04d%02d%02d-%02d%02d%02d",
                  pid, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    // Precise module blame from the fault address (fault→throw path), else the
    // top non-runtime backtrace frame.
    std::string blamed = fault_addr ? blame_addr(fault_addr) : blame_backtrace(frames, nframes);

    // Prefer a REAL Breakpad minidump when the writer hook is registered; the
    // .json sidecar is then written next to Breakpad's <uuid>.dmp. Otherwise fall
    // back to a text-backtrace .dmp beside a <stem>.json. Either way the FE finds
    // the sidecar via the "minidump: <path>.dmp" line printed at the end.
    std::string dmp_path, json_path, minidump_name;
    bool real_dump = false;
    if (auto w = minidump_writer_hook().load(std::memory_order_acquire)) {
        char buf[1024] = {0};
        if (w(dir.c_str(), buf, (int)sizeof buf) && buf[0]) {
            dmp_path = buf;
            std::filesystem::path p(dmp_path);
            minidump_name = p.filename().string();
            json_path = (p.parent_path() / (p.stem().string() + ".json")).string();
            real_dump = true;
        }
    }
    if (!real_dump) {
        dmp_path      = dir + "/" + stem + ".dmp";
        json_path     = dir + "/" + stem + ".json";
        minidump_name = std::string(stem) + ".dmp";
        // Text-backtrace .dmp stand-in (a human-readable, preservable stack).
        if (int fd = ::open(dmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); fd >= 0) {
            dprintf(fd, "xInsp2 backtrace (%s, signal/exception %s, module %s)\n",
                    XINSP2_VERSION, exc_name, blamed.c_str());
            if (frames && nframes > 0) backtrace_symbols_fd(const_cast<void**>(frames), nframes, fd);
            ::close(fd);
        }
    }

    // .json: byte-compatible with the Windows write_minidump sidecar the FE reads.
    char tsbuf[32];
    std::snprintf(tsbuf, sizeof tsbuf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    std::string out = "{\"version\":\"" XINSP2_VERSION "\",\"commit\":\"" XINSP2_COMMIT "\"";
    out += ",\"pid\":" + std::to_string(pid);
    out += ",\"timestamp\":"; json_escape(out, tsbuf);
    out += ",\"exception\":{\"name\":"; json_escape(out, exc_name);
    char addrbuf[40];
    std::snprintf(addrbuf, sizeof addrbuf, "\"0x%llx\"", (unsigned long long)(uintptr_t)fault_addr);
    out += ",\"address\":"; out += addrbuf;
    out += ",\"module\":"; json_escape(out, blamed.c_str());
    out += "}";
    {
        auto& c = ctx();
        out += ",\"context\":{";
        out += "\"last_cmd\":";       json_escape(out, c.last_cmd);
        out += ",\"last_script\":";   json_escape(out, c.last_script);
        out += ",\"last_instance\":"; json_escape(out, c.last_instance);
        out += ",\"last_plugin\":";   json_escape(out, c.last_plugin);
        out += ",\"last_phase\":";    json_escape(out, c.last_phase);
        out += ",\"last_status\":";   json_escape(out, c.last_status);
        out += ",\"last_run_id\":" + std::to_string(c.last_run_id);
        out += ",\"last_frame\":"  + std::to_string(c.last_frame);
        out += "}";
    }
    {
        out += ",\"culprit\":{";
        out += "\"instance\":"; json_escape(out, g_culprit.instance);
        out += ",\"plugin\":";  json_escape(out, g_culprit.plugin);
        out += ",\"folder\":";  json_escape(out, g_culprit.folder);
        out += ",\"dll\":";     json_escape(out, g_culprit.dll);
        out += "}";
    }
    out += ",\"threads\":[";
    {
        bool first = true;
        for (int i = 0; i < kMaxSlots; ++i) {
            uint32_t tid = g_slot_tid[i].load(std::memory_order_acquire);
            if (tid == 0) continue;
            auto& c = g_slots[i];
            if (!first) out += ",";
            first = false;
            out += "{\"thread_id\":" + std::to_string(tid);
            out += ",\"last_cmd\":";      json_escape(out, c.last_cmd);
            out += ",\"last_instance\":"; json_escape(out, c.last_instance);
            out += ",\"last_plugin\":";   json_escape(out, c.last_plugin);
            out += ",\"last_phase\":";    json_escape(out, c.last_phase);
            out += ",\"last_status\":";   json_escape(out, c.last_status);
            out += ",\"last_run_id\":" + std::to_string(c.last_run_id);
            out += ",\"last_frame\":"  + std::to_string(c.last_frame);
            out += "}";
        }
    }
    out += "]";
    out += ",\"minidump\":"; json_escape(out, minidump_name.c_str());
    out += "}\n";

    if (int fd = ::open(json_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); fd >= 0) {
        ssize_t w = ::write(fd, out.data(), out.size()); (void)w;
        ::close(fd);
    }

    std::fprintf(stderr, "[xinsp2] CRASH (%s) in %s — minidump: %s\n",
                 exc_name, blamed.c_str(), dmp_path.c_str());
    std::fflush(stderr);
    g_dump_in_progress.store(0, std::memory_order_release);
}

// std::terminate hook: the fault→seh_exception→uncaught path (and any uncaught
// C++ exception / noexcept violation) lands here on the faulting thread. Identify
// the in-flight exception, capture a backtrace, write the report, then die.
inline void posix_terminate_handler() {
    void* bt[64];
    int n = backtrace(bt, 64);
    const char* name = "TERMINATE";
    void* fault = xi::last_fault_addr();   // set by the seh handler on this thread
    try {
        if (auto e = std::current_exception()) std::rethrow_exception(e);
    } catch (const xi::seh_exception& se) {
        name = se.what();                  // ACCESS_VIOLATION / INT_DIVIDE_BY_ZERO / …
    } catch (const std::exception&) {
        name = "CPP_EXCEPTION";
    } catch (...) {
        name = "UNKNOWN_EXCEPTION";
    }
    write_crash_report_posix(name, fault, bt, n);
    std::fflush(stdout); std::fflush(stderr);
    std::abort();   // actually terminate (our SIGABRT handler already ran once → no re-entry)
}

inline void posix_signal_handler(int sig, siginfo_t* si, void*) {
    void* bt[64];
    int n = backtrace(bt, 64);
    write_crash_report_posix(signal_name(sig), si ? si->si_addr : nullptr, bt, n);
    std::signal(sig, SIG_DFL);
    std::raise(sig);   // re-raise so the process dies with the real signal disposition
}

// No dedicated fault stack needed here: the SIGABRT path does not exhaust the
// stack, and the SIGSEGV/stack-overflow altstack is owned by xi_seh.hpp's
// translator (install_seh_translator). Kept for API parity with the Windows side.
inline void reserve_fault_stack() {}

inline void install() {
    std::set_terminate(posix_terminate_handler);
    // SIGABRT (abort/assert) — the translator never touches it. No SA_ONSTACK:
    // abort doesn't overflow the stack. (SIGSEGV/FPE/BUS/ILL are owned by the seh
    // translator, which throws → the uncaught path routes through set_terminate.)
    struct sigaction sa; std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = posix_signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGABRT, &sa, nullptr);
    // Let the seh hard-exit path (recover_seh_stack_or_die) drain an in-flight
    // report before _Exit, mirroring the Windows H7 rendezvous.
    xi::seh_predump_drain_hook().store(+[]{ xi::crash::await_dump(2000); },
                                      std::memory_order_release);
}
#endif // _WIN32

} // namespace crash
} // namespace xi
