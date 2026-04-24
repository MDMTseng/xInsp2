//
// test_native.cpp — developer-side native tests for my_plugin.
//
// Two layers:
//   1. baseline_all_pass — runs the same 8 baseline tests the host runs
//      on first load. Passing here writes cert.json so the host won't
//      re-run them on next load.
//   2. my_plugin_* — your plugin-specific assertions. Add as many
//      XI_TEST(...) blocks as you want.
//
// Run: cmake builds my_plugin_test.exe; just execute it.
// Exit 0 = all passed.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_baseline.hpp>
#include <xi/xi_cert.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_test.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>

// CMake injects this. Falls back to "my_plugin.dll" in cwd.
#ifndef MY_PLUGIN_DLL_PATH
#define MY_PLUGIN_DLL_PATH "my_plugin.dll"
#endif

static HMODULE g_dll = nullptr;
static xi::baseline::PluginSymbols g_syms;
static xi_host_api g_host = xi::ImagePool::make_host_api();

static void load_dll() {
    if (g_dll) return;
    g_dll = LoadLibraryA(MY_PLUGIN_DLL_PATH);
    if (!g_dll) {
        std::fprintf(stderr, "failed to load %s (err %lu)\n",
                     MY_PLUGIN_DLL_PATH, GetLastError());
        std::exit(2);
    }
    g_syms = xi::baseline::load_symbols(g_dll);
    if (!g_syms.ok()) {
        std::fprintf(stderr, "%s missing required C ABI exports\n", MY_PLUGIN_DLL_PATH);
        std::exit(2);
    }
}

// --- Baseline: same gate the host runs on cert ---------------------------

XI_TEST(baseline_all_pass) {
    load_dll();
    auto summary = xi::baseline::run_all(g_syms, &g_host);
    for (auto& r : summary.results) {
        if (!r.passed) std::fprintf(stderr, "  baseline fail: %s: %s\n",
                                    r.name.c_str(), r.error.c_str());
    }
    XI_EXPECT(summary.all_passed);
    if (summary.all_passed) {
        auto folder = std::filesystem::path(MY_PLUGIN_DLL_PATH).parent_path();
        xi::cert::certify(folder, MY_PLUGIN_DLL_PATH, "my_plugin", g_syms, &g_host);
    }
}

// --- Plugin-specific tests (add your own below) -------------------------

XI_TEST(my_plugin_create_destroy) {
    load_dll();
    void* inst = g_syms.create(&g_host, "smoke");
    XI_EXPECT(inst != nullptr);
    g_syms.destroy(inst);
}

XI_TEST(my_plugin_get_def_is_json) {
    load_dll();
    void* inst = g_syms.create(&g_host, "j");
    char buf[4096];
    int n = g_syms.get_def(inst, buf, sizeof(buf));
    g_syms.destroy(inst);
    XI_EXPECT(n > 0);
    XI_EXPECT(buf[0] == '{');
}

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}
