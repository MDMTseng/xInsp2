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
#include <cstring>
#include <thread>

static std::atomic<int> g_in_process{0};
static std::atomic<int> g_release{0};
static std::atomic<int> g_prepare_done{0};
static std::atomic<int> g_commit_done{0};
static std::atomic<int> g_staged{0};
static std::atomic<int> g_active{0};

extern "C" {

XI_EXPORT void* xi_plugin_create(const xi_host_api*, const char*) { return (void*)1; }
XI_EXPORT void  xi_plugin_destroy(void*) {}

// Data plane = the xi.pack@1 door. Parks itself the same way the old
// xi_plugin_process did (hold the non-reentrant CallScope slot until released),
// then returns an empty answer — the test observes the parking, not the output.
static xi_pack_handle stage_pack_process(void*, xi_pack_handle) {
    g_in_process.store(1);
    while (g_release.load() == 0) std::this_thread::yield();   // hold the slot
    g_in_process.store(0);
    return XI_PACK_NULL;
}

XI_EXPORT const void* xi_plugin_get_interface(const char* id, uint32_t version) {
    if (id && version == 1u && std::strcmp(id, "xi.pack") == 0) {
        static const xi_pack_proc_v1 iface = { &stage_pack_process };
        return &iface;
    }
    return nullptr;
}

// Ungated by the host: touches ONLY the staging marker, never g_active.
XI_EXPORT int xi_plugin_prepare(void*, const char* /*def*/, const char* /*folder*/) {
    g_staged.fetch_add(1);
    g_prepare_done.store(1);
    return 0;
}

// Gated by the host: swaps staging → live.
XI_EXPORT void xi_plugin_commit(void*) {
    g_active.store(g_staged.load());
    g_commit_done.store(1);
}

XI_EXPORT int xi_plugin_get_def(void*, char* buf, int cap) {
    int n = std::snprintf(buf, (size_t)cap, "{\"active\":%d}", g_active.load());
    return n;
}
XI_EXPORT int xi_plugin_set_def(void*, const char*) { return 0; }

// --- test control hooks ---
XI_EXPORT int  stage_probe_in_process()  { return g_in_process.load(); }
XI_EXPORT void stage_probe_release()     { g_release.store(1); }
XI_EXPORT int  stage_probe_prepare_done(){ return g_prepare_done.load(); }
XI_EXPORT int  stage_probe_commit_done() { return g_commit_done.load(); }
XI_EXPORT void stage_probe_reset() {
    g_in_process.store(0); g_release.store(0);
    g_prepare_done.store(0); g_commit_done.store(0);
}

} // extern "C"
