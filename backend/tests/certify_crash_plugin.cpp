//
// certify_crash_plugin.cpp — Part III G1 fixture: a DLL that PASSES the ABI gate
// but whose factory faults (null dereference) the instant it is instantiated.
//
// This is the canonical "would have killed the backend at discovery" plugin. It
// exports a valid xi_plugin_abi_version so plugin_abi_compatible accepts it
// (reaching the factory call), then xi_plugin_create writes through a null
// pointer → an SEH access violation. Under --certify-plugin the throwaway child
// takes that fault (xi_crash_dump writes a minidump, the child terminates with
// the SEH code), the parent reads the abnormal exit as `crashed`, and
// scan_plugins skips + surfaces it.
//
// Raw C exports (no XI_PLUGIN_IMPL) so the loader resolves them with
// GetProcAddress exactly as production does — mirrors golden_plugin.cpp. Its
// data plane is the v12 xi.pack@1 door (xi_pack_proc_v1); the fault fires in the
// factory, so that door is never actually reached.
//
#include <cstdint>
#include <cstring>

#include <xi/xi_abi.h>        // XI_ABI_VERSION, xi_host_api, xi.pack@1 types

namespace {

// The pack data-plane door. The factory faults before an instance ever exists,
// so this is never reached in practice; on any non-crash path there is no
// resolved pack iface, so it returns XI_PACK_NULL.
static xi_pack_handle crash_pack_process(void* /*inst*/, xi_pack_handle /*input*/) {
    return XI_PACK_NULL;
}

} // namespace

extern "C" {

XI_EXPORT int xi_plugin_abi_version(void) {
    return XI_ABI_VERSION;   // pass the version gate
}

// The sole plugin data plane: the xi.pack@1 door. Published so the plugin is a
// well-formed v12 plugin; the crash happens in the factory before it is driven.
XI_EXPORT const void* xi_plugin_get_interface(const char* id, uint32_t version) {
    if (id && version == 1u && std::strcmp(id, "xi.pack") == 0) {
        static const xi_pack_proc_v1 iface = { &crash_pack_process };
        return &iface;
    }
    return nullptr;
}

XI_EXPORT void* xi_plugin_create(const xi_host_api* /*host*/, const char* /*name*/) {
    // Deliberate hard fault at instantiation — the exact DllMain/factory hazard
    // G1 isolates into a child process. `volatile` so the optimizer can't elide
    // the store into a trap.
    volatile int* boom = nullptr;
    *boom = 0xBADBEEF;
    return nullptr;   // unreachable
}

XI_EXPORT void xi_plugin_destroy(void* /*p*/) {}

} // extern "C"
