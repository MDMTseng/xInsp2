//
// certify_crash_plugin.cpp — Part III G1 fixture: a DLL that PASSES the ABI gate
// but whose factory faults (null dereference) the instant it is instantiated.
//
// This is the canonical "would have killed the backend at discovery" plugin. It
// exports a valid xi_plugin_abi_version + xi_yyjson_abi so plugin_abi_compatible
// accepts it (reaching the factory call), then xi_plugin_create writes through a
// null pointer → an SEH access violation. Under --certify-plugin the throwaway
// child takes that fault (xi_crash_dump writes a minidump, the child terminates
// with the SEH code), the parent reads the abnormal exit as `crashed`, and
// scan_plugins skips + surfaces it.
//
// Raw C exports (no XI_PLUGIN_IMPL) so the loader resolves them with
// GetProcAddress exactly as production does — mirrors golden_plugin.cpp.
//
#include <cstdint>

#include <xi/xi_abi.h>        // XI_ABI_VERSION, xi_host_api
#include <xi/xi_record.hpp>   // xi::yyjson_layout_stamp()

extern "C" {

__declspec(dllexport) int xi_plugin_abi_version(void) {
    return XI_ABI_VERSION;   // pass the version gate
}

__declspec(dllexport) uint32_t xi_yyjson_abi(void) {
    return xi::yyjson_layout_stamp();   // pass the yyjson layout gate
}

__declspec(dllexport) void* xi_plugin_create(const xi_host_api* /*host*/, const char* /*name*/) {
    // Deliberate hard fault at instantiation — the exact DllMain/factory hazard
    // G1 isolates into a child process. `volatile` so the optimizer can't elide
    // the store into a trap.
    volatile int* boom = nullptr;
    *boom = 0xBADBEEF;
    return nullptr;   // unreachable
}

__declspec(dllexport) void xi_plugin_destroy(void* /*p*/) {}

__declspec(dllexport) void xi_plugin_process(void* /*p*/, const xi_record* /*in*/,
                                             xi_record_out* /*out*/) {}

} // extern "C"
