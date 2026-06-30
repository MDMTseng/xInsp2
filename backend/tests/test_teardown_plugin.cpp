//
// test_teardown_plugin.cpp — a minimal, real xi C-ABI plugin DLL used ONLY by
// test_plugin_teardown.cpp. It exports just the symbols the host loader resolves
// at load time (ABI version, yyjson layout stamp, factory) so plugin_abi_compatible
// passes and a c_factory is found. It never instantiates anything heavy — the
// teardown test only loads + frees the DLL, it doesn't create instances.
//
#include <cstdint>
#include <xi/xi_abi.h>        // XI_ABI_VERSION, xi_host_api
#include <xi/xi_record.hpp>   // xi::yyjson_layout_stamp()

extern "C" __declspec(dllexport) int xi_plugin_abi_version(void) {
    return XI_ABI_VERSION;
}
extern "C" __declspec(dllexport) uint32_t xi_yyjson_abi(void) {
    return xi::yyjson_layout_stamp();
}
extern "C" __declspec(dllexport) void* xi_plugin_create(const xi_host_api*, const char*) {
    // Never dereferenced by the teardown test; non-null so the loader's
    // factory-resolution succeeds.
    return reinterpret_cast<void*>(0x1);
}
extern "C" __declspec(dllexport) void xi_plugin_destroy(void*) {}
