//
// test_prepare_concurrency.cpp — proves the ABI v7 gating contract (task #70)
// through the REAL CAbiInstanceAdapter on a NON-reentrant instance (cap=1):
//
//   * prepare() is UNGATED — it completes while a process() is in flight (holding
//     the only slot). This is what keeps a heavy background load off the pipeline.
//   * commit() is GATED — it BLOCKS until process() frees the slot, so the live
//     swap lands in a no-process window.
//
// The stage_probe fixture parks process() on a release flag so we control exactly
// when the slot is held/freed.
//
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_pack_abi.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#ifndef STAGE_PROBE_DLL
#define STAGE_PROBE_DLL "stage_probe.dll"
#endif

using xi::CAbiInstanceAdapter;
using clk = std::chrono::steady_clock;

template <class Fn>
static bool wait_until(Fn pred, int timeout_ms = 2000) {
    auto deadline = clk::now() + std::chrono::milliseconds(timeout_ms);
    while (clk::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

int main() {
    HMODULE dll = LoadLibraryA(STAGE_PROBE_DLL);
    if (!dll) { std::fprintf(stderr, "load %s failed (err %lu)\n", STAGE_PROBE_DLL, GetLastError()); return 2; }

    auto create        = reinterpret_cast<void* (*)(const xi_host_api*, const char*)>(GetProcAddress(dll, "xi_plugin_create"));
    auto in_process    = reinterpret_cast<int  (*)()>(GetProcAddress(dll, "stage_probe_in_process"));
    auto release       = reinterpret_cast<void (*)()>(GetProcAddress(dll, "stage_probe_release"));
    auto prepare_done  = reinterpret_cast<int  (*)()>(GetProcAddress(dll, "stage_probe_prepare_done"));
    auto commit_done   = reinterpret_cast<int  (*)()>(GetProcAddress(dll, "stage_probe_commit_done"));
    auto reset         = reinterpret_cast<void (*)()>(GetProcAddress(dll, "stage_probe_reset"));
    if (!create || !in_process || !release || !prepare_done || !commit_done || !reset) {
        std::fprintf(stderr, "missing fixture exports\n"); return 2;
    }

    reset();
    void* inst = create(nullptr, "p");
    // NON-reentrant → CallScope cap = 1.
    CAbiInstanceAdapter adapter("p0", "stage_probe", dll, inst, /*reentrant=*/false, /*max_concurrency=*/0);

    // Thread P: enter the pack door and PARK there, holding the cap=1 slot.
    std::thread proc([&] {
        xi_pack_handle out = adapter.run_pack_door(XI_PACK_NULL);
        if (out != XI_PACK_NULL) xi::pack_v1_iface()->release(out);
    });
    if (!wait_until([&] { return in_process() == 1; })) {
        std::fprintf(stderr, "FAIL: process() never entered\n"); release(); proc.join(); return 1;
    }

    // (1) prepare() while process is parked → must COMPLETE (ungated).
    std::thread prep([&] { adapter.prepare("{\"value\":1}", ""); });
    bool prep_ok = wait_until([&] { return prepare_done() == 1; });
    prep.join();
    bool still_parked_after_prepare = (in_process() == 1);

    // (2) commit() while process is parked → must BLOCK (gated). Run it on a
    // thread and confirm it does NOT finish while the slot is held.
    std::thread comm([&] { adapter.commit(); });
    bool commit_blocked = !wait_until([&] { return commit_done() == 1; }, 150)
                          && in_process() == 1;

    // Release process() → it frees the slot; commit() must now go through.
    release();
    proc.join();
    bool commit_after_release = wait_until([&] { return commit_done() == 1; });
    comm.join();

    int rc = 0;
    auto check = [&](bool cond, const char* msg) {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", msg);
        if (!cond) rc = 1;
    };
    std::printf("=== ABI v7 prepare/commit gating (non-reentrant, cap=1) ===\n");
    check(prep_ok && still_parked_after_prepare,
          "prepare() completed while a process() was in flight (UNGATED)");
    check(commit_blocked,
          "commit() blocked while process() held the slot (GATED)");
    check(commit_after_release,
          "commit() went through once process() freed the slot");
    std::printf(rc == 0 ? "PASS\n" : "FAIL\n");
    return rc;
}
