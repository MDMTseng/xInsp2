//
// test_counter.cpp — developer-side tests for the counter plugin.
//
// Plugins are trusted (no certification gate), so testing is optional and
// owned by you. This shows the pattern: stand up the host side of the
// xi.pack@1 data plane (ImagePool::make_host_api + install_pack_abi — the
// same pair the backend wires), resolve the plugin's C-ABI exports from the
// built DLL, and drive its pack door (xi_plugin_get_interface("xi.pack", 1))
// to assert behaviour (starts at zero, increments, reset, persistence across
// instances) with xi_test. THE CUT (v12): there is no xi_plugin_process /
// xi_record — the door is the sole data plane.
//
// Build produces counter_test.exe. Run it; exit code 0 = all pass.
//

#include <xi/xi_abi.h>
#include <xi/xi_image_pool.hpp>   // ImagePool::make_host_api (the real pool)
#include <xi/xi_pack_abi.hpp>     // install_pack_abi + pack_v1_iface (host pack ABI)
#include <xi/xi_test.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdlib>
#include <cstring>
#include <string>

// Path to the built plugin DLL (CMake sets COUNTER_DLL_PATH).
#ifndef COUNTER_DLL_PATH
#define COUNTER_DLL_PATH "counter.dll"
#endif

// Plugin C-ABI exports, resolved by name from the DLL (see xi_abi.h). The
// data plane is the xi.pack@1 door behind xi_plugin_get_interface.
struct Syms {
    xi_plugin_create_fn        create    = nullptr;
    xi_plugin_destroy_fn       destroy   = nullptr;
    xi_plugin_exchange_fn      exchange  = nullptr;
    xi_plugin_get_def_fn       get_def   = nullptr;
    xi_plugin_set_def_fn       set_def   = nullptr;
    xi_plugin_get_interface_fn get_iface = nullptr;
};
static HMODULE g_dll = nullptr;
static Syms g_syms;
static const xi_pack_proc_v1* g_door = nullptr;
static xi_host_api g_host;

static void load_dll() {
    if (g_dll) return;
    // Host side of the pack plane FIRST: make_host_api gives the pool-backed
    // host_api; install_pack_abi publishes xi_pack_v1 into its
    // get_interface("xi.pack", 1) slot so the plugin's builders resolve.
    g_host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();

    g_dll = LoadLibraryA(COUNTER_DLL_PATH);
    if (!g_dll) {
        std::fprintf(stderr, "failed to load %s (err %lu)\n",
                     COUNTER_DLL_PATH, GetLastError());
        std::exit(2);
    }
    g_syms.create    = (xi_plugin_create_fn)       GetProcAddress(g_dll, "xi_plugin_create");
    g_syms.destroy   = (xi_plugin_destroy_fn)      GetProcAddress(g_dll, "xi_plugin_destroy");
    g_syms.exchange  = (xi_plugin_exchange_fn)     GetProcAddress(g_dll, "xi_plugin_exchange");
    g_syms.get_def   = (xi_plugin_get_def_fn)      GetProcAddress(g_dll, "xi_plugin_get_def");
    g_syms.set_def   = (xi_plugin_set_def_fn)      GetProcAddress(g_dll, "xi_plugin_set_def");
    g_syms.get_iface = (xi_plugin_get_interface_fn)GetProcAddress(g_dll, "xi_plugin_get_interface");
    if (!g_syms.create || !g_syms.destroy || !g_syms.get_iface) {
        std::fprintf(stderr, "DLL missing required C ABI exports\n");
        std::exit(2);
    }
    g_door = static_cast<const xi_pack_proc_v1*>(g_syms.get_iface("xi.pack", 1));
    if (!g_door || !g_door->process) {
        std::fprintf(stderr, "DLL has no xi.pack@1 door (XI_PLUGIN_PACK_DOOR missing?)\n");
        std::exit(2);
    }
}

// Drive one door call with an empty input pack; return the "count" entry from
// the result (-1 if absent). Owns + releases both handles.
static long long door_count_once(void* inst) {
    const xi_pack_v1* fi = xi::pack_v1_iface();
    xi_pack_handle in = fi->builder_seal(fi->builder_new());   // empty pack
    xi_pack_handle out = g_door->process(inst, in);
    long long count = -1;
    if (out != XI_PACK_NULL) {
        int64_t v = 0;
        if (fi->get_i64(out, "count", &v)) count = (long long)v;
        fi->release(out);
    }
    fi->release(in);
    return count;
}

// --- Plugin-specific tests ---------------------------------------------

XI_TEST(counter_starts_at_zero) {
    load_dll();
    void* inst = g_syms.create(&g_host, "t1");
    XI_EXPECT(inst != nullptr);
    char buf[256];
    int n = g_syms.get_def(inst, buf, sizeof(buf));
    XI_EXPECT(n > 0);
    std::string def(buf);
    XI_EXPECT(def.find("\"count\":0") != std::string::npos);
    g_syms.destroy(inst);
}

XI_TEST(counter_increments_on_process) {
    load_dll();
    void* inst = g_syms.create(&g_host, "t2");
    for (int i = 1; i <= 5; ++i)
        XI_EXPECT(door_count_once(inst) == i);
    g_syms.destroy(inst);
}

XI_TEST(counter_reset_command_zeros_it) {
    load_dll();
    void* inst = g_syms.create(&g_host, "t3");
    for (int i = 0; i < 7; ++i) door_count_once(inst);
    char rsp[256];
    g_syms.exchange(inst, R"({"command":"reset"})", rsp, sizeof(rsp));
    char buf[256];
    g_syms.get_def(inst, buf, sizeof(buf));
    XI_EXPECT(std::string(buf).find("\"count\":0") != std::string::npos);
    g_syms.destroy(inst);
}

XI_TEST(counter_state_survives_reload) {
    load_dll();
    // Bump counter on instance A
    void* a = g_syms.create(&g_host, "a");
    for (int i = 0; i < 3; ++i) door_count_once(a);
    char saved[256];
    g_syms.get_def(a, saved, sizeof(saved));
    g_syms.destroy(a);

    // Create fresh instance B, restore saved def — count must match
    void* b = g_syms.create(&g_host, "b");
    g_syms.set_def(b, saved);
    char recovered[256];
    g_syms.get_def(b, recovered, sizeof(recovered));
    g_syms.destroy(b);

    XI_EXPECT(std::string(saved) == std::string(recovered));
    XI_EXPECT(std::string(recovered).find("\"count\":3") != std::string::npos);
}

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}
