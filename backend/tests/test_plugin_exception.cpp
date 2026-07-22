//
// test_plugin_exception.cpp — B2: a C++ exception thrown from a plugin must be
// contained. Two boundaries, two disciplines (see throwing_plugin.cpp):
//   * The XI_PLUGIN_IMPL LIFECYCLE exports (set_def/get_def/exchange) catch the
//     throw IN-PLUGIN and return a safe sentinel — it must NOT unwind across those
//     extern "C" exports. Proven both (A) calling the raw C-ABI exports directly
//     and (B) through the real host adapter (CAbiInstanceAdapter). Reaching the
//     line AFTER each call is itself the assertion that nothing terminated.
//   * THE CUT (v12) DATA-PLANE door is the RAW xi.pack@1 door. It does NOT catch
//     in-plugin; the throw escapes across the boundary and the HOST contains it —
//     proven by driving run_pack_door under the test's own try/catch (C).
//
// (Destructor-throw is intentionally NOT tested: C++ destructors are implicitly
// noexcept, so a throwing dtor calls std::terminate before the macro's catch can
// run. The destroy guard only helps an explicitly noexcept(false) dtor — a rare
// case not worth a fixture.)
//
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_pack_abi.hpp>   // install_pack_abi / pack_v1_iface (the xi.pack@1 door)
#include <xi/xi_abi.h>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef THROWING_DLL
#define THROWING_DLL "throwing_plugin.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

int main() {
    std::printf("[test] B2 — plugin exports catch throws (no unwind across the C ABI)\n");

    HMODULE dll = LoadLibraryA(THROWING_DLL);
    if (!dll) {
        std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", THROWING_DLL, GetLastError());
        return 1;
    }

    auto create  = reinterpret_cast<void* (*)(const xi_host_api*, const char*)>(
        GetProcAddress(dll, "xi_plugin_create"));
    auto destroy = reinterpret_cast<void (*)(void*)>(GetProcAddress(dll, "xi_plugin_destroy"));
    auto p_set   = reinterpret_cast<int (*)(void*, const char*)>(GetProcAddress(dll, "xi_plugin_set_def"));
    auto p_get   = reinterpret_cast<int (*)(void*, char*, int)>(GetProcAddress(dll, "xi_plugin_get_def"));
    auto p_exch  = reinterpret_cast<int (*)(void*, const char*, char*, int)>(GetProcAddress(dll, "xi_plugin_exchange"));
    CHECK(create && destroy && p_set && p_get && p_exch);
    if (g_failures) { FreeLibrary(dll); return 1; }

    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();   // publish the xi.pack@1 builder the door plane needs
    void* inst = create(&host, "boom0");
    CHECK(inst != nullptr);

    // ---- (A) Direct C-ABI: each export catches its throw, returns a sentinel ----
    // set_def throws -> caught -> -1 (config rejected)
    CHECK(p_set(inst, "{}") == -1);
    // get_def throws -> caught -> 0 (empty def)
    char buf[64];
    CHECK(p_get(inst, buf, (int)sizeof buf) == 0);
    // exchange throws -> caught -> 0 (empty response)
    char rsp[64];
    CHECK(p_exch(inst, "{\"command\":\"x\"}", rsp, (int)sizeof rsp) == 0);
    // (The data-plane door throw is proven in (C) below: THE CUT removed the raw
    // xi_plugin_process export, and the xi.pack@1 door is deliberately RAW — it
    // does NOT catch in-plugin, so it's driven through the host adapter.)

    destroy(inst);

    // ---- (B) Same throws survive through the REAL host adapter ----
    // CAbiInstanceAdapter wraps calls in CallScope + an ImagePool OwnerGuard; the
    // export's in-plugin catch means the adapter (and thus the host) never unwinds.
    void* inst2 = create(&host, "boom1");
    CHECK(inst2 != nullptr);
    {
        xi::CAbiInstanceAdapter ad("boom1", "throwing_plugin", dll, inst2,
                                   /*reentrant=*/false, /*max_concurrency=*/0);
        // The export's in-plugin catch returns a sentinel; the adapter normalises
        // an empty/failed def or response to its neutral "{}". The point is that
        // the throw never unwinds the adapter (no terminate) and yields a safe value.
        CHECK(ad.set_def("{}") == false);            // export -1 -> adapter false, no throw
        CHECK(ad.get_def() == "{}");                 // export 0 -> adapter neutral "{}"
        CHECK(ad.exchange("{\"command\":\"x\"}") == "{}");  // ditto, no throw

        // ---- (C) The RAW data-plane door throw escapes across the boundary; the
        // HOST contains it. run_pack_door calls the plugin's xi.pack@1 door
        // directly (no in-plugin trampoline), so the throw propagates out here —
        // reaching the CHECK after the catch is the proof the host boundary held.
        bool door_threw = false;
        try {
            xi_pack_handle out = ad.run_pack_door(XI_PACK_NULL);
            if (out != XI_PACK_NULL) xi::pack_v1_iface()->release(out);
        } catch (...) {
            door_threw = true;
        }
        CHECK(door_threw);
    }  // ~adapter -> xi_plugin_destroy, no crash

    FreeLibrary(dll);

    if (g_failures) { std::fprintf(stderr, "\nFAILED (%d)\n", g_failures); return 1; }
    std::printf("\nALL TESTS PASSED\n");
    return 0;
}
