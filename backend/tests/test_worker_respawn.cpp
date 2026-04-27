//
// test_worker_respawn.cpp — Phase 2.7 auto-respawn validation.
//
// Spawns an isolated instance the same way open_project would, kills
// the worker process from outside, and verifies the next RPC detects
// the death, transparently respawns a fresh worker (carrying the
// adapter's saved set_def state across the gap), and the call
// succeeds. Also exercises the rate limiter.
//

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>

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

static bool kill_pid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    BOOL ok = TerminateProcess(h, 1);
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
    return ok != 0;
}

int main() {
    char self[MAX_PATH]; GetModuleFileNameA(nullptr, self, MAX_PATH);
    fs::path here       = fs::path(self).parent_path();
    fs::path worker_exe = here / "xinsp-worker.exe";
    fs::path plugin_dll = here / "test_worker_plugin.dll";
    if (!fs::exists(worker_exe) || !fs::exists(plugin_dll)) {
        std::fprintf(stderr, "missing prereqs\n");
        return 2;
    }

    char shm_name[64];
    std::snprintf(shm_name, sizeof(shm_name), "xinsp2-shm-resp-%lu",
                  (unsigned long)GetCurrentProcessId());
    auto shm_owned = xi::ShmRegion::create(shm_name, 16ull * 1024 * 1024);
    xi::ImagePool::set_shm_region(&shm_owned);

    xi::ProcessInstanceAdapter inst("cam0", "test_doubler",
                                    worker_exe, plugin_dll, shm_name);
    DWORD pid_before = inst.worker_pid();
    CHECK(pid_before != 0);
    std::fprintf(stderr, "[test] initial worker pid=%lu\n", (unsigned long)pid_before);

    // Cache a non-empty def via set_def so we can verify the respawn
    // restore path. (Test plugin's set_def returns 0 = success regardless,
    // and stash is host-side, so this exercises the saved_def_ flow.)
    CHECK(inst.set_def("{\"phase\":\"2.7\"}"));

    // Sanity: pre-kill RPC works.
    {
        std::string r = inst.exchange("hello");
        CHECK(r.find("doubler") != std::string::npos);
    }

    // ---- Respawn #1: kill from outside, RPC should auto-recover -----
    std::fprintf(stderr, "[test] killing worker pid=%lu...\n",
                 (unsigned long)pid_before);
    CHECK(kill_pid(pid_before));
    // Give the OS a beat to tear the pipe down so the next RPC sees EOF
    // not a write success.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    {
        std::string r = inst.exchange("hello");
        CHECK(r.find("doubler") != std::string::npos);
    }
    DWORD pid_after = inst.worker_pid();
    CHECK(pid_after != 0);
    CHECK(pid_after != pid_before);
    CHECK(inst.respawn_count() == 1);
    CHECK(!inst.is_dead());
    std::fprintf(stderr, "[test] respawned: %lu → %lu (count=%d)\n",
                 (unsigned long)pid_before, (unsigned long)pid_after,
                 inst.respawn_count());

    // Process round-trip on the respawned worker — proves it's fully
    // wired (CREATE happened, plugin is alive, SHM is the same region).
    {
        static xi_host_api host = xi::ImagePool::make_host_api();
        xi_image_handle in_h = host.shm_create_image(16, 8, 1);
        CHECK(in_h != 0);
        uint8_t* px = host.image_data(in_h);
        for (int i = 0; i < 16 * 8; ++i) px[i] = (uint8_t)(i & 0x7F);

        xi_record_image rec{ "frame", in_h };
        xi_record in_rec{}; in_rec.images = &rec; in_rec.image_count = 1; in_rec.json = "{}";
        xi_record_out out_rec{};
        std::string err;
        bool ok = inst.process_via_rpc(&in_rec, &out_rec, &err);
        CHECK(ok);
        if (ok && out_rec.image_count == 1) {
            const uint8_t* op = host.image_data(out_rec.images[0].handle);
            int wrong = 0;
            for (int i = 0; i < 16 * 8; ++i) {
                int v = (i & 0x7F) * 2; if (v > 255) v = 255;
                if (op[i] != (uint8_t)v) ++wrong;
            }
            CHECK(wrong == 0);
            host.image_release(out_rec.images[0].handle);
        }
        host.image_release(in_h);
    }

    // ---- Rate-limit: kill + RPC three more times → fourth fails -----
    // Counter already at 1. Kill + recover x2 lands at 3 (cap). The 4th
    // kill leaves the adapter dead.
    for (int i = 2; i <= 3; ++i) {
        DWORD pid = inst.worker_pid();
        CHECK(kill_pid(pid));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::string r = inst.exchange("hello");
        CHECK(r.find("doubler") != std::string::npos);
        CHECK(inst.respawn_count() == i);
    }
    // 4th kill — respawn cap reached; RPC returns "{}" (defaulted) and
    // the adapter is dead.
    DWORD pid = inst.worker_pid();
    CHECK(kill_pid(pid));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::string r = inst.exchange("hello");
    CHECK(r == "{}");          // safe-default since respawn refused
    CHECK(inst.is_dead());
    std::fprintf(stderr, "[test] rate limit hit at %d respawns, instance now dead\n",
                 inst.respawn_count());

    if (g_failures == 0) {
        std::fprintf(stderr, "test_worker_respawn: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_worker_respawn: %d FAIL\n", g_failures);
    return 1;
}
