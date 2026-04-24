#pragma once
//
// xi_baseline.hpp — baseline behavioral tests every plugin must pass.
//
// These probe the C ABI surface of an already-loaded DLL. They catch the
// classes of bug that would wreck the host: crashes on empty input, lost
// state across get_def/set_def, data races under concurrent process(),
// use-after-free on destroy, etc.
//
// Bump BASELINE_VERSION whenever the test set changes — old certs will
// be invalidated and plugins re-tested on next load.
//

#include "xi_abi.h"
#include "xi_image_pool.hpp"
#include "cJSON.h"

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace xi::baseline {

static constexpr int BASELINE_VERSION = 1;

struct TestResult {
    std::string name;
    bool        passed;
    std::string error;
    double      ms;
};

struct Summary {
    bool                    all_passed;
    int                     pass_count;
    int                     fail_count;
    double                  total_ms;
    std::vector<TestResult> results;
};

// Loaded from the DLL once, reused across tests.
struct PluginSymbols {
    xi_plugin_create_fn   create   = nullptr;
    xi_plugin_destroy_fn  destroy  = nullptr;
    xi_plugin_process_fn  process  = nullptr;
    xi_plugin_exchange_fn exchange = nullptr;
    xi_plugin_get_def_fn  get_def  = nullptr;
    xi_plugin_set_def_fn  set_def  = nullptr;

    bool ok() const { return create && destroy; }
};

inline PluginSymbols load_symbols(HMODULE dll) {
    PluginSymbols s;
    s.create   = (xi_plugin_create_fn)  GetProcAddress(dll, "xi_plugin_create");
    s.destroy  = (xi_plugin_destroy_fn) GetProcAddress(dll, "xi_plugin_destroy");
    s.process  = (xi_plugin_process_fn) GetProcAddress(dll, "xi_plugin_process");
    s.exchange = (xi_plugin_exchange_fn)GetProcAddress(dll, "xi_plugin_exchange");
    s.get_def  = (xi_plugin_get_def_fn) GetProcAddress(dll, "xi_plugin_get_def");
    s.set_def  = (xi_plugin_set_def_fn) GetProcAddress(dll, "xi_plugin_set_def");
    return s;
}

// --- Individual tests ----------------------------------------------------

// All tests take (symbols, host) and return true on pass, setting err on fail.
using TestFn = bool (*)(const PluginSymbols&, const xi_host_api*, std::string&);

inline bool t_factory_create_destroy(const PluginSymbols& s, const xi_host_api* h, std::string& err) {
    void* inst = s.create(h, "baseline_0");
    if (!inst) { err = "create returned null"; return false; }
    s.destroy(inst);
    return true;
}

inline bool t_get_def_returns_valid_json(const PluginSymbols& s, const xi_host_api* h, std::string& err) {
    if (!s.get_def) return true; // optional
    void* inst = s.create(h, "baseline_0");
    if (!inst) { err = "create failed"; return false; }
    char buf[8192];
    int n = s.get_def(inst, buf, sizeof(buf));
    s.destroy(inst);
    if (n < 0) { err = "get_def wanted " + std::to_string(-n) + " bytes"; return false; }
    cJSON* p = cJSON_Parse(buf);
    if (!p) { err = "get_def output is not valid JSON"; return false; }
    cJSON_Delete(p);
    return true;
}

inline bool t_get_set_def_roundtrip(const PluginSymbols& s, const xi_host_api* h, std::string& err) {
    if (!s.get_def || !s.set_def) return true;
    void* a = s.create(h, "baseline_a");
    if (!a) { err = "create failed"; return false; }
    char buf1[8192], buf2[8192];
    int n1 = s.get_def(a, buf1, sizeof(buf1));
    if (n1 < 0) { s.destroy(a); err = "get_def too large"; return false; }
    void* b = s.create(h, "baseline_b");
    s.set_def(b, buf1);
    int n2 = s.get_def(b, buf2, sizeof(buf2));
    s.destroy(a); s.destroy(b);
    if (n2 < 0 || std::strcmp(buf1, buf2) != 0) {
        err = "get_def→set_def→get_def not stable";
        return false;
    }
    return true;
}

inline bool t_exchange_valid_json(const PluginSymbols& s, const xi_host_api* h, std::string& err) {
    if (!s.exchange) return true;
    void* inst = s.create(h, "baseline_0");
    if (!inst) { err = "create failed"; return false; }
    char rsp[8192];
    int n = s.exchange(inst, "{}", rsp, sizeof(rsp));
    s.destroy(inst);
    if (n < 0) return true; // size request is fine
    if (n == 0) return true; // empty is fine
    cJSON* p = cJSON_Parse(rsp);
    if (!p) { err = "exchange response is not valid JSON"; return false; }
    cJSON_Delete(p);
    return true;
}

inline bool t_process_empty_input(const PluginSymbols& s, const xi_host_api* h, std::string& err) {
    if (!s.process) return true;
    void* inst = s.create(h, "baseline_0");
    if (!inst) { err = "create failed"; return false; }
    xi_record in; in.json = "{}"; in.images = nullptr; in.image_count = 0;
    xi_record_out out; xi_record_out_init(&out);
    s.process(inst, &in, &out);
    xi_record_out_free(&out);
    s.destroy(inst);
    return true; // if we got here, no crash
}

inline bool t_concurrent_process(const PluginSymbols& s, const xi_host_api* h, std::string& err) {
    if (!s.process) return true;
    void* inst = s.create(h, "baseline_0");
    if (!inst) { err = "create failed"; return false; }

    constexpr int THREADS = 4;
    constexpr int ITERS   = 50;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < THREADS; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < ITERS; ++i) {
                xi_record in; in.json = "{}"; in.images = nullptr; in.image_count = 0;
                xi_record_out out; xi_record_out_init(&out);
                try { s.process(inst, &in, &out); }
                catch (...) { failures.fetch_add(1); }
                xi_record_out_free(&out);
            }
        });
    }
    for (auto& w : workers) w.join();
    s.destroy(inst);
    if (failures.load() > 0) {
        err = std::to_string(failures.load()) + " concurrent process() calls threw";
        return false;
    }
    return true;
}

inline bool t_concurrent_mixed(const PluginSymbols& s, const xi_host_api* h, std::string& err) {
    if (!s.process || !s.exchange) return true;
    void* inst = s.create(h, "baseline_0");
    if (!inst) { err = "create failed"; return false; }

    constexpr int THREADS = 4;
    constexpr int ITERS   = 50;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < THREADS; ++t) {
        bool does_exchange = (t % 2 == 0);
        workers.emplace_back([&, does_exchange] {
            for (int i = 0; i < ITERS; ++i) {
                try {
                    if (does_exchange) {
                        char rsp[4096];
                        s.exchange(inst, R"({"command":"get_status"})", rsp, sizeof(rsp));
                    } else {
                        xi_record in; in.json = "{}"; in.images = nullptr; in.image_count = 0;
                        xi_record_out out; xi_record_out_init(&out);
                        s.process(inst, &in, &out);
                        xi_record_out_free(&out);
                    }
                } catch (...) { failures.fetch_add(1); }
            }
        });
    }
    for (auto& w : workers) w.join();
    s.destroy(inst);
    if (failures.load() > 0) {
        err = std::to_string(failures.load()) + " concurrent ops threw";
        return false;
    }
    return true;
}

inline bool t_many_create_destroy(const PluginSymbols& s, const xi_host_api* h, std::string& err) {
    for (int i = 0; i < 20; ++i) {
        void* inst = s.create(h, ("x" + std::to_string(i)).c_str());
        if (!inst) { err = "create " + std::to_string(i) + " failed"; return false; }
        s.destroy(inst);
    }
    return true;
}

inline const std::vector<std::pair<const char*, TestFn>>& all_tests() {
    static const std::vector<std::pair<const char*, TestFn>> tests = {
        {"factory_create_destroy",     t_factory_create_destroy},
        {"get_def_returns_valid_json", t_get_def_returns_valid_json},
        {"get_set_def_roundtrip",      t_get_set_def_roundtrip},
        {"exchange_valid_json",        t_exchange_valid_json},
        {"process_empty_input",        t_process_empty_input},
        {"concurrent_process",         t_concurrent_process},
        {"concurrent_mixed",           t_concurrent_mixed},
        {"many_create_destroy",        t_many_create_destroy},
    };
    return tests;
}

inline Summary run_all(const PluginSymbols& s, const xi_host_api* h) {
    Summary summary{true, 0, 0, 0, {}};
    auto overall_start = std::chrono::steady_clock::now();
    for (auto& [name, fn] : all_tests()) {
        TestResult r{name, true, "", 0};
        auto t0 = std::chrono::steady_clock::now();
        try { r.passed = fn(s, h, r.error); }
        catch (const std::exception& e) { r.passed = false; r.error = std::string("exception: ") + e.what(); }
        catch (...) { r.passed = false; r.error = "unknown exception"; }
        auto t1 = std::chrono::steady_clock::now();
        r.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (r.passed) ++summary.pass_count;
        else { ++summary.fail_count; summary.all_passed = false; }
        summary.results.push_back(std::move(r));
    }
    auto overall_end = std::chrono::steady_clock::now();
    summary.total_ms = std::chrono::duration<double, std::milli>(overall_end - overall_start).count();
    return summary;
}

} // namespace xi::baseline
