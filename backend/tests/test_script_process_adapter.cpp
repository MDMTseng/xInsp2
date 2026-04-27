//
// test_script_process_adapter.cpp — Phase 3.8 verification.
//
// Drives ScriptProcessAdapter the way service_main would: start +
// inspect+snapshot + stop. Validates that the adapter wraps the
// runner cleanly and the use_* callbacks (when a handler is installed)
// still work transparently across the process boundary.
//

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include <xi/xi_image_pool.hpp>
#include <xi/xi_script_process_adapter.hpp>
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
    fs::path runner_exe = here / "xinsp-script-runner.exe";
    fs::path script_dll = here / "test_script_runner_dll.dll";
    if (!fs::exists(runner_exe) || !fs::exists(script_dll)) return 2;

    char shm_name[64];
    std::snprintf(shm_name, sizeof(shm_name), "xinsp2-shm-spa-%lu",
                  (unsigned long)GetCurrentProcessId());
    auto shm = xi::ShmRegion::create(shm_name, 8 * 1024 * 1024);
    xi::ImagePool::set_shm_region(&shm);

    xi::script::ScriptProcessAdapter ad;
    std::string err;
    bool started = ad.start(runner_exe, script_dll, shm_name, err);
    CHECK(started);
    if (!started) { std::fprintf(stderr, "[test] start: %s\n", err.c_str()); return 3; }
    CHECK(ad.ok());
    CHECK(ad.runner_pid() != 0);
    std::fprintf(stderr, "[test] runner spawned pid=%lu\n",
                 (unsigned long)ad.runner_pid());

    // Install a session handler so the script's use_* callbacks
    // (exercised inside inspect_entry) reach back to us.
    ad.set_handler([&](uint32_t type, const std::vector<uint8_t>& payload)
                   -> std::vector<uint8_t> {
        if (type == xi::ipc::RPC_USE_EXCHANGE) {
            xi::ipc::Reader r(payload);
            std::string name = r.str();
            std::string cmd  = r.str();
            std::string rsp = "{\"who\":\"" + name + "\",\"cmd_len\":"
                            + std::to_string(cmd.size()) + "}";
            xi::ipc::Writer w; w.bytes(rsp.data(), rsp.size());
            return w.buf();
        }
        if (type == xi::ipc::RPC_USE_GRAB) {
            xi::ipc::Reader r(payload);
            std::string name = r.str();
            (void)r.u32();
            uint64_t h = shm.alloc_image(4, 4, 1);
            std::memset(shm.data(h), 0x55, 16);
            xi::ipc::Writer w; w.u64(h);
            return w.buf();
        }
        // use_process not exercised by this test (no input set) — fall
        // through to error to catch any unexpected path.
        throw std::runtime_error("unhandled rpc " + std::to_string(type));
    });

    // First run.
    std::string vars1;
    bool ok1 = ad.inspect_and_snapshot(13, vars1, err);
    CHECK(ok1);
    if (!ok1) { std::fprintf(stderr, "[test] run1: %s\n", err.c_str()); return 4; }
    std::fprintf(stderr, "[test] vars1: %s\n", vars1.c_str());
    CHECK(vars1.find("\"value\":13") != std::string::npos);
    CHECK(vars1.find("\"value\":26") != std::string::npos);
    // exchange/grab callbacks should have fired.
    CHECK(vars1.find("\"exch_rc\"") != std::string::npos);
    CHECK(vars1.find("\"grabbed_handle\"") != std::string::npos);

    // Second run with reset between (handled by runner's RPC_SCRIPT_RUN
    // which calls reset before inspect).
    std::string vars2;
    CHECK(ad.inspect_and_snapshot(100, vars2, err));
    CHECK(vars2.find("\"value\":200") != std::string::npos);

    // Stop tears down the runner cleanly.
    ad.stop();
    CHECK(!ad.ok());

    if (g_failures == 0) {
        std::fprintf(stderr, "test_script_process_adapter: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_script_process_adapter: %d FAIL\n", g_failures);
    return 1;
}
