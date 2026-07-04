//
// test_teardown_plugin.cpp — a minimal, real xi C-ABI plugin DLL used ONLY by
// test_plugin_teardown.cpp. It exports just the symbols the host loader resolves
// at load time (ABI version, factory, and the xi.pack@1 data-plane door) so
// plugin_abi_compatible passes and a c_factory is found. It never instantiates
// anything heavy — the teardown test only loads + frees the DLL, it doesn't
// create instances or drive the door.
//
#include <cstdint>
#include <cstring>
#include <xi/xi_abi.h>        // XI_ABI_VERSION, xi_host_api, xi.pack@1 door types

// THE CUT (v12): the data plane is the xi.pack@1 door. This fixture never runs
// it (the teardown test only load/free-checks the DLL), so the door returns the
// XI_PACK_NULL hard-failure sentinel; publishing it just makes the plugin a
// well-formed pack-speaking plugin for the loader.
static xi_pack_handle teardown_pack_process(void* /*inst*/, xi_pack_handle /*in*/) {
    return XI_PACK_NULL;
}

extern "C" __declspec(dllexport) int xi_plugin_abi_version(void) {
    return XI_ABI_VERSION;
}
extern "C" __declspec(dllexport)
const void* xi_plugin_get_interface(const char* id, uint32_t version) {
    if (id && version == 1u && std::strcmp(id, "xi.pack") == 0) {
        static const xi_pack_proc_v1 iface = { &teardown_pack_process };
        return &iface;
    }
    return nullptr;
}
extern "C" __declspec(dllexport) void* xi_plugin_create(const xi_host_api*, const char*) {
    // Never dereferenced by the teardown test; non-null so the loader's
    // factory-resolution succeeds.
    return reinterpret_cast<void*>(0x1);
}
extern "C" __declspec(dllexport) void xi_plugin_destroy(void*) {}
