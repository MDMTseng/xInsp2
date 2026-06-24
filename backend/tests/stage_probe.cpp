//
// stage_probe.cpp — fixture for the ungated-prepare vs gated-commit contract
// (test_prepare_concurrency.cpp, ABI v7 / task #70).
//
// process() parks itself (sets in_process, spins until the test releases it) so
// it HOLDS the non-reentrant CallScope slot (cap=1) for as long as the test
// wants. While it's parked, the test calls prepare() and commit() through the
// real adapter and checks who gets through:
//   * prepare() is called UNGATED by the adapter → it must complete even though a
//     process() is in flight (it touches only the staging marker).
//   * commit() is called GATED → it must BLOCK until process() frees the slot.
//
// Raw C exports (no XI_PLUGIN_IMPL) + DLL-global atomics, since the test drives a
// single instance and needs to peek/poke the gate from outside.
//
#include <xi/xi_abi.h>

#include <atomic>
#include <thread>

static std::atomic<int> g_in_process{0};
static std::atomic<int> g_release{0};
static std::atomic<int> g_prepare_done{0};
static std::atomic<int> g_commit_done{0};
static std::atomic<int> g_staged{0};
static std::atomic<int> g_active{0};

extern "C" {

__declspec(dllexport) void* xi_plugin_create(const xi_host_api*, const char*) { return (void*)1; }
__declspec(dllexport) void  xi_plugin_destroy(void*) {}

__declspec(dllexport) void xi_plugin_process(void*, const xi_record*, xi_record_out* out) {
    g_in_process.store(1);
    while (g_release.load() == 0) std::this_thread::yield();   // hold the slot
    g_in_process.store(0);
    if (out) out->image_count = 0;
}

// Ungated by the host: touches ONLY the staging marker, never g_active.
__declspec(dllexport) int xi_plugin_prepare(void*, const char* /*def*/, const char* /*folder*/) {
    g_staged.fetch_add(1);
    g_prepare_done.store(1);
    return 0;
}

// Gated by the host: swaps staging → live.
__declspec(dllexport) void xi_plugin_commit(void*) {
    g_active.store(g_staged.load());
    g_commit_done.store(1);
}

__declspec(dllexport) int xi_plugin_get_def(void*, char* buf, int cap) {
    int n = std::snprintf(buf, (size_t)cap, "{\"active\":%d}", g_active.load());
    return n;
}
__declspec(dllexport) int xi_plugin_set_def(void*, const char*) { return 0; }

// --- test control hooks ---
__declspec(dllexport) int  stage_probe_in_process()  { return g_in_process.load(); }
__declspec(dllexport) void stage_probe_release()     { g_release.store(1); }
__declspec(dllexport) int  stage_probe_prepare_done(){ return g_prepare_done.load(); }
__declspec(dllexport) int  stage_probe_commit_done() { return g_commit_done.load(); }
__declspec(dllexport) void stage_probe_reset() {
    g_in_process.store(0); g_release.store(0);
    g_prepare_done.store(0); g_commit_done.store(0);
}

} // extern "C"
