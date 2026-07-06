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
#endif

#include "xi_seh.hpp"   // xi::seh_exception (on_terminate classifies it)

#include <atomic>
#include <chrono>
#include <csignal>
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
#else
    uint32_t tid = 1;  // TODO(linux): pthread_self()-derived id
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

// Map an instruction pointer to "<module>+0x<offset>" by scanning loaded
// modules. Used in the crash filter to point at which DLL (script vs plugin vs
// xinsp-backend itself) was executing.
inline std::string blame_module(void* addr) {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return "<unknown>";
    int n = (int)(needed / sizeof(HMODULE));
    for (int i = 0; i < n; ++i) {
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        auto base = (uintptr_t)mi.lpBaseOfDll;
        if ((uintptr_t)addr < base || (uintptr_t)addr >= base + mi.SizeOfImage) continue;
        char name[MAX_PATH];
        GetModuleFileNameA(mods[i], name, sizeof(name));
        const char* slash = std::strrchr(name, '\\');
        std::string out = (slash ? slash + 1 : name);
        char off[64];
        std::snprintf(off, sizeof(off), "+0x%llx", (unsigned long long)((uintptr_t)addr - base));
        return out + off;
    }
    return "<unknown>";
}

// JSON-escape a path segment in-place (writes into out). Tiny copy of
// xp::json_escape_into to keep this filter free of any nontrivial dep.
inline void json_escape(std::string& out, const char* s) {
    out.push_back('"');
    for (; *s; ++s) {
        char c = *s;
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out.push_back(c);
        }
    }
    out.push_back('"');
}

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

    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "xinsp2" / "crashdumps";
    std::error_code ec;
    fs::create_directories(dir, ec);
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
    auto dmp_path  = (dir / (std::string(stem) + ".dmp")).string();
    auto json_path = (dir / (std::string(stem) + ".json")).string();

    DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    void* addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
    std::string blamed = blame_module(addr);

    // 1. Minidump
    HANDLE h = CreateFileA(dmp_path.c_str(), GENERIC_WRITE, 0, nullptr,
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

    // 2. JSON sidecar — what the next-startup report path reads.
    std::string out = "{\"version\":\""  XINSP2_VERSION "\""
                      ",\"commit\":\""  XINSP2_COMMIT "\""
                      ",\"pid\":" + std::to_string(GetCurrentProcessId())
                    + ",\"thread_id\":" + std::to_string(GetCurrentThreadId());
    char tsbuf[64];
    std::snprintf(tsbuf, sizeof(tsbuf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    out += ",\"timestamp\":";
    json_escape(out, tsbuf);
    out += ",\"exception\":{\"code\":";
    char codebuf[24];
    std::snprintf(codebuf, sizeof(codebuf), "\"0x%08X\"", code);
    out += codebuf;
    out += ",\"name\":";
    json_escape(out, exception_name(code));
    char addrbuf[40];
    std::snprintf(addrbuf, sizeof(addrbuf), "\"0x%llx\"", (unsigned long long)addr);
    out += ",\"address\":"; out += addrbuf;
    out += ",\"module\":"; json_escape(out, blamed.c_str());
    out += "}";
    // `context` = the faulting thread's breadcrumb (the handler runs on
    // the faulting thread, so ctx() is its slot). Back-compat with the
    // existing report reader which expects this object.
    uint32_t fault_tid = (uint32_t)GetCurrentThreadId();
    {
        auto& c = ctx();
        out += ",\"context\":{";
        out += "\"last_cmd\":";      json_escape(out, c.last_cmd);
        out += ",\"last_script\":";  json_escape(out, c.last_script);
        out += ",\"last_instance\":";json_escape(out, c.last_instance);
        out += ",\"last_plugin\":";  json_escape(out, c.last_plugin);
        out += ",\"last_phase\":";   json_escape(out, c.last_phase);
        out += ",\"last_status\":";  json_escape(out, c.last_status);
        out += ",\"last_run_id\":" + std::to_string(c.last_run_id);
        out += ",\"last_frame\":"  + std::to_string(c.last_frame);
        out += "}";
    }
    // `culprit` (Part III G2.1) = the process-global plugin/instance the host
    // last entered, plus that plugin's folder + dll. The FE reads this to
    // attribute the death to a plugin and (cross-checked against `module`)
    // quarantine it via G1's .xi_certify.json. Empty fields = not attributable.
    {
        out += ",\"culprit\":{";
        out += "\"instance\":";  json_escape(out, g_culprit.instance);
        out += ",\"plugin\":";   json_escape(out, g_culprit.plugin);
        out += ",\"folder\":";   json_escape(out, g_culprit.folder);
        out += ",\"dll\":";      json_escape(out, g_culprit.dll);
        out += "}";
    }
    // `threads` = every claimed breadcrumb slot, so a multi-dispatch crash shows
    // what ALL concurrent inspects were doing, not just the faulting one.
    // `faulting:true` flags the culprit.
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
            out += ",\"faulting\":" + std::string(tid == fault_tid ? "true" : "false");
            out += ",\"last_cmd\":";     json_escape(out, c.last_cmd);
            out += ",\"last_instance\":";json_escape(out, c.last_instance);
            out += ",\"last_plugin\":";  json_escape(out, c.last_plugin);
            out += ",\"last_phase\":";   json_escape(out, c.last_phase);
            out += ",\"last_status\":";  json_escape(out, c.last_status);
            out += ",\"last_run_id\":" + std::to_string(c.last_run_id);
            out += ",\"last_frame\":"  + std::to_string(c.last_frame);
            out += "}";
        }
    }
    out += "]";
    out += ",\"minidump\":";
    json_escape(out, (std::string(stem) + ".dmp").c_str());
    out += "}\n";

    HANDLE jh = CreateFileA(json_path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (jh != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(jh, out.data(), (DWORD)out.size(), &wrote, nullptr);
        CloseHandle(jh);
    }

    std::fprintf(stderr, "[xinsp2] CRASH 0x%08X (%s) in %s — minidump: %s\n",
                 code, exception_name(code), blamed.c_str(), dmp_path.c_str());
    std::fflush(stderr);
    // Dump + sidecar are on disk. Release the H3 flag so a hard-exit path blocked
    // in await_dump() (H7) resumes promptly instead of waiting out its full bound.
    g_dump_in_progress.store(0, std::memory_order_release);
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
    set(ctx().last_cmd, sizeof(ctx().last_cmd), "terminate");
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
    set(ctx().last_cmd, sizeof(ctx().last_cmd), cause);
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
        std::string blamed = blame_module(addr);
        std::fprintf(stderr,
            "[xinsp2] VEH first-chance 0x%08X (%s) thread %lu at %s\n",
            code, exception_name(code),
            (unsigned long)GetCurrentThreadId(), blamed.c_str());
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

#else  // !_WIN32
// TODO(linux): dbghelp/SEH have no portable analogue yet. Stub so the host
// compiles; the breadcrumb model above still works.
inline void reserve_fault_stack() {}
inline void install() {}
#endif // _WIN32

} // namespace crash
} // namespace xi
