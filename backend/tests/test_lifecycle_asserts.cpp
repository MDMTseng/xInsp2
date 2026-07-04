//
// test_lifecycle_asserts.cpp — G3.2 (core_fix_plan-2026-07 §18): the debug-build
// lifecycle×thread contract assertions in CAbiInstanceAdapter.
//
// The G3.1 contract (docs/guides/write-a-plugin.md § "Plugin lifecycle & threading
// contract") is now machine-checked in Debug. The genuinely illegal, gate-defeating
// transition is a SAME-THREAD re-entry into a non-reentrant instance's gated export
// — CallScope (cap=1) would deadlock on the slot the thread already holds. The guard
// converts that silent hang into a catchable violation. We drive the two plan-named
// cases (plus process→process) via reentry_probe, whose process() re-enters the real
// adapter on the host thread:
//   • process → set_def   — "set_def() during process() on a non-reentrant instance"
//   • process → commit    — "process() before commit() (out-of-order lifecycle)"
//   • process → process   — nested re-entry (same family)
// And we prove the guard does NOT false-positive on the LEGAL paths:
//   • a clean, non-nested sequence
//   • cross-thread concurrent set_def vs process (gate-serialized; legal)
//   • the same re-entry on a REENTRANT instance (gate lifted; plugin owns locking)
//
// The illegal-path checks need the debug guard, so they install a non-aborting
// violation handler that RECORDS the trap (instead of std::abort) and let the
// method BAIL — no real abort to catch in-process. Under NDEBUG the guard compiles
// out; those sections are skipped and only the legal sequence is exercised.
//
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_pack_abi.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifndef REENTRY_PROBE_DLL
#define REENTRY_PROBE_DLL "reentry_probe.dll"
#endif

using xi::CAbiInstanceAdapter;

static int g_failures = 0;
#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

// ---- violation recorder (installed only in Debug) -------------------------
static std::vector<std::string> g_violations;
#ifndef NDEBUG
static void record_violation(const char* what, const char* detail) {
    g_violations.emplace_back(std::string(what) + ": " + (detail ? detail : ""));
    std::printf("    [trapped] %s (%s)\n", what, detail ? detail : "");
}
#endif

using CreateFn = void* (*)(const xi_host_api*, const char*);
using ArmFn    = void   (*)(void (*)(void*), void*);

// Re-entry trampoline: invoked from inside the fixture's process(), on the host
// thread, to re-enter the SAME adapter through a gated export.
struct ReentryCtx { CAbiInstanceAdapter* a; int which; };
static void reentry_trampoline(void* p) {
    auto* c = static_cast<ReentryCtx*>(p);
    switch (c->which) {
        case 0: c->a->set_def("{}"); break;                 // process → set_def
        case 1: c->a->commit();      break;                 // process → commit
        case 2: {                                           // process → process
            xi_pack_handle out = c->a->run_pack_door(XI_PACK_NULL);
            if (out != XI_PACK_NULL) xi::pack_v1_iface()->release(out);
        } break;
    }
}

static void run_process(CAbiInstanceAdapter& a) {
    xi_pack_handle out = a.run_pack_door(XI_PACK_NULL);
    if (out != XI_PACK_NULL) xi::pack_v1_iface()->release(out);
}

// ---- legal, non-nested sequence: no violation -----------------------------
static void test_legal_sequence(HMODULE dll, CreateFn create) {
    SECTION("legal lifecycle sequence (non-reentrant) trips nothing");
    void* inst = create(nullptr, "p");
    CAbiInstanceAdapter a("p0", "reentry_probe", dll, inst, /*reentrant=*/false);
    g_violations.clear();
    a.set_def("{}");
    (void)a.get_def();
    run_process(a);
    a.commit();
    run_process(a);
    CHECK(g_violations.empty());
}

// ---- legal cross-thread concurrency: gate-serialized, NOT flagged ---------
static void test_cross_thread_legal(HMODULE dll, CreateFn create) {
    SECTION("cross-thread set_def vs process (gate-serialized) is NOT flagged");
    void* inst = create(nullptr, "p");
    CAbiInstanceAdapter a("p0", "reentry_probe", dll, inst, /*reentrant=*/false);
    g_violations.clear();
    std::atomic<bool> stop{false};
    std::thread proc([&] { for (int i = 0; i < 20000 && !stop.load(); ++i) run_process(a); stop.store(true); });
    std::thread setr([&] { while (!stop.load()) a.set_def("{}"); });
    proc.join(); setr.join();
    CHECK(g_violations.empty());   // two threads is legal; only same-thread re-entry is illegal
}

#ifndef NDEBUG
// ---- illegal same-thread re-entry on a NON-reentrant instance -------------
static void test_illegal_reentry(HMODULE dll, CreateFn create, ArmFn arm,
                                 int which, const char* label) {
    SECTION(label);
    void* inst = create(nullptr, "p");
    CAbiInstanceAdapter a("p0", "reentry_probe", dll, inst, /*reentrant=*/false);
    g_violations.clear();
    ReentryCtx ctx{&a, which};
    arm(&reentry_trampoline, &ctx);
    run_process(a);   // process() body re-enters the adapter on this thread → trap
    CHECK(g_violations.size() == 1);
    if (!g_violations.empty())
        CHECK(g_violations[0].rfind("non-reentrant re-entry", 0) == 0);
}

// ---- same re-entry on a REENTRANT instance: legal, NOT flagged ------------
static void test_reentrant_ok(HMODULE dll, CreateFn create, ArmFn arm) {
    SECTION("reentrant instance: same-thread re-entry is legal (gate lifted)");
    void* inst = create(nullptr, "p");
    CAbiInstanceAdapter a("p0", "reentry_probe", dll, inst, /*reentrant=*/true,
                          /*max_concurrency=*/0);
    g_violations.clear();
    ReentryCtx ctx{&a, 0};   // process → set_def, but reentrant ⇒ allowed
    arm(&reentry_trampoline, &ctx);
    run_process(a);
    CHECK(g_violations.empty());
}
#endif // NDEBUG

int main() {
    HMODULE dll = LoadLibraryA(REENTRY_PROBE_DLL);
    if (!dll) { std::fprintf(stderr, "load %s failed (err %lu)\n", REENTRY_PROBE_DLL, GetLastError()); return 2; }
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(dll, "xi_plugin_create"));
    auto arm    = reinterpret_cast<ArmFn>(GetProcAddress(dll, "reentry_probe_arm"));
    if (!create || !arm) { std::fprintf(stderr, "missing fixture exports\n"); return 2; }

    std::printf("=== G3.2 lifecycle contract asserts ===\n");

#ifndef NDEBUG
    xi::lifecycle_violation_handler() = &record_violation;   // record, don't abort
#endif

    test_legal_sequence(dll, create);
    test_cross_thread_legal(dll, create);

#ifndef NDEBUG
    test_illegal_reentry(dll, create, arm, 0,
        "ILLEGAL: set_def() during process() on a non-reentrant instance");
    test_illegal_reentry(dll, create, arm, 1,
        "ILLEGAL: commit() during process() (out-of-order lifecycle)");
    test_illegal_reentry(dll, create, arm, 2,
        "ILLEGAL: process() re-entered during process() (out-of-order lifecycle)");
    test_reentrant_ok(dll, create, arm);
#else
    std::printf("[test] NDEBUG build: guard compiled out; illegal-path checks skipped\n");
#endif

    if (g_failures == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
