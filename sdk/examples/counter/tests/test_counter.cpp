//
// test_counter.cpp — developer-side tests for the counter plugin.
//
// Shows two layers:
//   1. Baseline tests — the exact same ones the backend runs for cert.
//      Passing these guarantees the plugin won't be quarantined on load.
//   2. Plugin-specific tests — behavior unique to counter (increment,
//      reset, persistence across instances).
//
// Build produces counter_test.exe. Run it; exit code 0 = all pass.
// If baseline tests pass here, it writes cert.json, saving the backend
// from re-running them on first load.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_baseline.hpp>
#include <xi/xi_cert.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_test.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <filesystem>
#include <cstdlib>

// Path to the built plugin DLL (CMake sets COUNTER_DLL_PATH).
#ifndef COUNTER_DLL_PATH
#define COUNTER_DLL_PATH "counter.dll"
#endif

static HMODULE g_dll = nullptr;
static xi::baseline::PluginSymbols g_syms;
static xi_host_api g_host = xi::ImagePool::make_host_api();

static void load_dll() {
    if (g_dll) return;
    g_dll = LoadLibraryA(COUNTER_DLL_PATH);
    if (!g_dll) {
        std::fprintf(stderr, "failed to load %s (err %lu)\n",
                     COUNTER_DLL_PATH, GetLastError());
        std::exit(2);
    }
    g_syms = xi::baseline::load_symbols(g_dll);
    if (!g_syms.ok()) {
        std::fprintf(stderr, "DLL missing required C ABI exports\n");
        std::exit(2);
    }
}

// --- Baseline tests (run against the DLL via shared helper) -------------

XI_TEST(baseline_all_pass) {
    load_dll();
    auto summary = xi::baseline::run_all(g_syms, &g_host);
    if (!summary.all_passed) {
        for (auto& r : summary.results) {
            if (!r.passed) std::fprintf(stderr, "  baseline fail: %s: %s\n",
                                        r.name.c_str(), r.error.c_str());
        }
        XI_EXPECT(summary.all_passed);
    }
    // On success, write cert so the backend doesn't re-run these.
    std::filesystem::path folder = std::filesystem::path(COUNTER_DLL_PATH).parent_path();
    xi::cert::certify(folder, COUNTER_DLL_PATH, "counter", g_syms, &g_host);
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
    xi_record in; in.json = "{}"; in.images = nullptr; in.image_count = 0;

    for (int i = 1; i <= 5; ++i) {
        xi_record_out out; xi_record_out_init(&out);
        g_syms.process(inst, &in, &out);
        std::string s = out.json ? out.json : "";
        xi_record_out_free(&out);
        std::string expect = "\"count\":" + std::to_string(i);
        XI_EXPECT(s.find(expect) != std::string::npos);
    }
    g_syms.destroy(inst);
}

XI_TEST(counter_reset_command_zeros_it) {
    load_dll();
    void* inst = g_syms.create(&g_host, "t3");
    xi_record in; in.json = "{}"; in.images = nullptr; in.image_count = 0;
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
    xi_record in; in.json = "{}"; in.images = nullptr; in.image_count = 0;
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
