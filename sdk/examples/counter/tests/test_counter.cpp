//
// test_counter.cpp — developer-side tests for the counter plugin.
//
// Plugins are trusted (no certification gate), so testing is optional and
// owned by you. This shows the pattern: resolve the plugin's C-ABI exports
// from the built DLL, then assert its behaviour (starts at zero, increments,
// reset, persistence across instances) with xi_test.
//
// Build produces counter_test.exe. Run it; exit code 0 = all pass.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_test.hpp>
#include "yyjson.h"

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdlib>
#include <cstring>
#include <string>

// Current ABI (v6): an xi_record carries its JSON in data/len (doc == NULL),
// not a `.json` member; an xi_record_out hands JSON back in data/len, or in
// out_doc when the in-process zero-copy doc path was taken. These two helpers
// hide that so the tests below read cleanly.
static xi_record make_in(const char* json) {
    xi_record in{};
    in.data = reinterpret_cast<const uint8_t*>(json);
    in.len  = static_cast<int32_t>(std::strlen(json));
    in.images = nullptr; in.image_count = 0;
    return in;
}
static std::string out_json(const xi_record_out& out) {
    if (out.out_doc) {
        size_t n = 0;
        char* s = yyjson_mut_write(reinterpret_cast<yyjson_mut_doc*>(out.out_doc), 0, &n);
        std::string r = s ? std::string(s, n) : "";
        if (s) free(s);
        return r;
    }
    return out.data ? std::string(reinterpret_cast<const char*>(out.data), static_cast<size_t>(out.len)) : "";
}

// Path to the built plugin DLL (CMake sets COUNTER_DLL_PATH).
#ifndef COUNTER_DLL_PATH
#define COUNTER_DLL_PATH "counter.dll"
#endif

// Plugin C-ABI exports, resolved by name from the DLL (see xi_abi.h).
struct Syms {
    xi_plugin_create_fn   create   = nullptr;
    xi_plugin_destroy_fn  destroy  = nullptr;
    xi_plugin_process_fn  process  = nullptr;
    xi_plugin_exchange_fn exchange = nullptr;
    xi_plugin_get_def_fn  get_def  = nullptr;
    xi_plugin_set_def_fn  set_def  = nullptr;
};
static HMODULE g_dll = nullptr;
static Syms g_syms;
static xi_host_api g_host = xi::ImagePool::make_host_api();

static void load_dll() {
    if (g_dll) return;
    g_dll = LoadLibraryA(COUNTER_DLL_PATH);
    if (!g_dll) {
        std::fprintf(stderr, "failed to load %s (err %lu)\n",
                     COUNTER_DLL_PATH, GetLastError());
        std::exit(2);
    }
    g_syms.create   = (xi_plugin_create_fn)  GetProcAddress(g_dll, "xi_plugin_create");
    g_syms.destroy  = (xi_plugin_destroy_fn) GetProcAddress(g_dll, "xi_plugin_destroy");
    g_syms.process  = (xi_plugin_process_fn) GetProcAddress(g_dll, "xi_plugin_process");
    g_syms.exchange = (xi_plugin_exchange_fn)GetProcAddress(g_dll, "xi_plugin_exchange");
    g_syms.get_def  = (xi_plugin_get_def_fn) GetProcAddress(g_dll, "xi_plugin_get_def");
    g_syms.set_def  = (xi_plugin_set_def_fn) GetProcAddress(g_dll, "xi_plugin_set_def");
    if (!g_syms.create || !g_syms.destroy || !g_syms.process) {
        std::fprintf(stderr, "DLL missing required C ABI exports\n");
        std::exit(2);
    }
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
    xi_record in = make_in("{}");

    for (int i = 1; i <= 5; ++i) {
        xi_record_out out; xi_record_out_init(&out);
        g_syms.process(inst, &in, &out);
        std::string s = out_json(out);
        xi_record_out_free(&out);
        std::string expect = "\"count\":" + std::to_string(i);
        XI_EXPECT(s.find(expect) != std::string::npos);
    }
    g_syms.destroy(inst);
}

XI_TEST(counter_reset_command_zeros_it) {
    load_dll();
    void* inst = g_syms.create(&g_host, "t3");
    xi_record in = make_in("{}");
    for (int i = 0; i < 7; ++i) {
        xi_record_out out; xi_record_out_init(&out);
        g_syms.process(inst, &in, &out);
        xi_record_out_free(&out);
    }
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
    xi_record in = make_in("{}");
    for (int i = 0; i < 3; ++i) {
        xi_record_out out; xi_record_out_init(&out);
        g_syms.process(a, &in, &out);
        xi_record_out_free(&out);
    }
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
