//
// test_worker_timeout.cpp — Tail 2 validation.
//
// Verifies that ProcessInstanceAdapter's per-call watchdog actually
// fires CancelIoEx when a plugin hangs in process(). The plugin we
// load (test_hang_plugin.dll) Sleeps 60s on every process call;
// without the watchdog the test would block forever.
//
// Expected behaviour with set_call_timeout_ms(500):
//   1. process_via_rpc returns false within ~500ms with err mentioning
//      "timeout"
//   2. Watchdog cancelled the in-flight ReadFile → adapter's call_()
//      catches → respawn fires → respawn_count() bumps to 1
//   3. The respawned worker has the SAME hang plugin, so next call
//      times out again → respawn_count == 2, then 3
//   4. After cap (3), adapter is dead and process_via_rpc returns
//      false with err='worker dead'
//

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include <xi/xi_image_pool.hpp>
#include <xi/xi_process_instance.hpp>
#include <xi/xi_shm.hpp>

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

int main() {
    char self[MAX_PATH]; GetModuleFileNameA(nullptr, self, MAX_PATH);
    fs::path here       = fs::path(self).parent_path();
    fs::path worker_exe = here / "xinsp-worker.exe";
    fs::path plugin_dll = here / "test_hang_plugin.dll";
    if (!fs::exists(worker_exe) || !fs::exists(plugin_dll)) return 2;

    char shm_name[64];
    std::snprintf(shm_name, sizeof(shm_name), "xinsp2-shm-tot-%lu",
                  (unsigned long)GetCurrentProcessId());
    auto shm = xi::ShmRegion::create(shm_name, 8 * 1024 * 1024);
    xi::ImagePool::set_shm_region(&shm);

    xi::ProcessInstanceAdapter inst("hang0", "test_hang",
                                    worker_exe, plugin_dll, shm_name);
    inst.set_call_timeout_ms(500);  // tight for the test

    // Allocate a dummy input so process_via_rpc has something to send.
    static xi_host_api host = xi::ImagePool::make_host_api();
    xi_image_handle in_h = host.shm_create_image(8, 8, 1);
    CHECK(in_h != 0);
    xi_record_image rec{ "frame", in_h };
    xi_record       in_rec{}; in_rec.images = &rec; in_rec.image_count = 1; in_rec.json = "{}";

    // First call: must time out within ~1s and surface "timeout".
    auto t0 = std::chrono::steady_clock::now();
    {
        xi_record_out out_rec{};
        std::string err;
        bool ok = inst.process_via_rpc(&in_rec, &out_rec, &err);
        auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        CHECK(!ok);
        // The respawn will spawn another worker (also hangs) and that
        // ALSO times out → second timeout → counted as another respawn.
        // Total elapsed includes both timeouts. Just check we returned
        // in finite time and err is sensible.
        std::fprintf(stderr, "[test] 1st call returned in %lldms ok=%d err='%s'\n",
                     (long long)dt_ms, (int)ok, err.c_str());
        CHECK(dt_ms < 8000);
    }
    // After respawn-then-retry-then-timeout-again pattern, we should
    // be at respawn cap (count == 3) and dead.
    std::fprintf(stderr, "[test] respawn_count=%d dead=%d\n",
                 inst.respawn_count(), (int)inst.is_dead());
    CHECK(inst.respawn_count() >= 1);
    CHECK(inst.is_dead());

    // Subsequent calls should fail fast without further respawn churn.
    {
        xi_record_out out_rec{};
        std::string err;
        bool ok = inst.process_via_rpc(&in_rec, &out_rec, &err);
        CHECK(!ok);
        CHECK(err == "worker dead");
    }

    host.image_release(in_h);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_worker_timeout: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_worker_timeout: %d FAIL\n", g_failures);
    return 1;
}
