//
// stale_plugin.cpp — a DELIBERATELY STALE xi C-ABI plugin fixture for the
// Phase-4 break test. It reports a PRE-v11 ABI version (v9 or v10, chosen by the
// STALE_PLUGIN_ABI compile definition), i.e. a plugin built against the OLD
// shm-bearing xi_host_api layout. After Phase 4 raised XI_ABI_MIN_COMPAT to 11
// such a plugin MUST be REFUSED at load: its compiled-in view of the table still
// expects the removed shm_* pointers, so handing it the v11 table would mis-offset
// every call. test_golden_plugin.cpp loads this through the real ABI gate
// (plugin_abi_compatible) and asserts a clean refuse with a min-compat reason —
// proving the authorized break actually bites.
//
// Intentionally does NOT include xi_abi.h or pin against XI_ABI_MIN_COMPAT (the
// golden's static_assert would reject a below-floor pin). It is a raw C-export
// stand-in for an old binary that predates the break.
//
#include <cstdint>

#ifndef STALE_PLUGIN_ABI
#define STALE_PLUGIN_ABI 10   // default: the immediately-previous ABI (v10)
#endif

extern "C" {

// The stale version this fixture claims — below the v11 min-compat floor.
__declspec(dllexport) int xi_plugin_abi_version(void) {
    return STALE_PLUGIN_ABI;
}

// A minimal create so the DLL looks like a real plugin (never reached: the ABI
// gate refuses before the loader resolves the factory).
__declspec(dllexport) void* xi_plugin_create(const void* /*host*/, const char* /*name*/) {
    return nullptr;
}

__declspec(dllexport) void xi_plugin_destroy(void* /*inst*/) {}

} // extern "C"
