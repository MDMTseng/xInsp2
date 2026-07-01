//
// reentry_probe.cpp — fixture for the G3.2 debug lifecycle-assert test
// (test_lifecycle_asserts.cpp). Its process() invokes a test-installed reentry
// callback ONCE, on the SAME thread, so the harness can drive an illegal
// same-thread re-entry (process → set_def / commit / process) into the real
// CAbiInstanceAdapter and prove the debug lifecycle guard TRAPS it — instead of
// the CallScope gate silently deadlocking on the cap=1 slot this thread holds.
//
// Raw C exports (no XI_PLUGIN_IMPL): the test drives one instance and needs to
// arm the reentry hook from outside. xi_plugin_commit is exported so the adapter's
// commit_fn_ is non-null and commit() reaches the guard.
//
#include <xi/xi_abi.h>

#include <atomic>
#include <cstdio>

typedef void (*xi_reentry_fn)(void* ctx);
static std::atomic<xi_reentry_fn> g_reentry{nullptr};
static void* g_ctx = nullptr;

extern "C" {

__declspec(dllexport) void* xi_plugin_create(const xi_host_api*, const char*) { return (void*)1; }
__declspec(dllexport) void  xi_plugin_destroy(void*) {}

__declspec(dllexport) void xi_plugin_process(void*, const xi_record*, xi_record_out* out) {
    // Fire the armed reentry ONCE, on THIS (host) thread, mid-process().
    if (xi_reentry_fn f = g_reentry.exchange(nullptr)) f(g_ctx);
    if (out) out->image_count = 0;
}

__declspec(dllexport) int  xi_plugin_set_def(void*, const char*) { return 0; }
__declspec(dllexport) int  xi_plugin_get_def(void*, char* buf, int cap) {
    if (cap > 2) { buf[0] = '{'; buf[1] = '}'; buf[2] = 0; return 2; }
    return 0;
}
__declspec(dllexport) void xi_plugin_commit(void*) {}

// --- test control hook: run f(ctx) once during the next process() ---
__declspec(dllexport) void reentry_probe_arm(xi_reentry_fn f, void* ctx) {
    g_ctx = ctx;
    g_reentry.store(f);
}

} // extern "C"
