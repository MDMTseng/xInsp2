//
// test_set_def_race.cpp — does the host let a realtime set_def race an in-flight
// process() on the same instance?
//
// Drives the REAL CAbiInstanceAdapter (the CallScope admission gate) around the
// unprotected race_probe fixture, with two threads hammering one instance:
//   * thread P: calls process() in a tight loop (the "camera streaming" side)
//   * thread S: calls set_def() in a tight loop (the "operator tunes a param" side)
// The fixture's set_def writes its two config halves non-atomically, so a torn
// read is observable ONLY if process() ran concurrently with set_def.
//
//   Round 1 — non-reentrant (default): cap=1, host serializes ALL entry points
//             → process() can never run during set_def → expect 0 torn reads.
//   Round 2 — reentrant (unlimited):   no host gate → set_def races process()
//             → expect torn reads (the plugin would have to protect itself).
//
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_pack_abi.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifndef RACE_PROBE_DLL
#define RACE_PROBE_DLL "race_probe.dll"
#endif

using xi::CAbiInstanceAdapter;

static long long run_round(HMODULE dll, bool reentrant) {
    auto create = reinterpret_cast<void* (*)(const xi_host_api*, const char*)>(
        GetProcAddress(dll, "xi_plugin_create"));
    void* inst = create(nullptr, "probe");
    // max_concurrency = 0 → reentrant means UNLIMITED (no gate); non-reentrant
    // ignores it and forces cap = 1.
    CAbiInstanceAdapter adapter("probe0", "race_probe", dll, inst, reentrant, /*max_concurrency=*/0);

    // XINSP2_STRESS_SCALE multiplies the process()/set_def churn for the
    // high-iteration `set_def_race_heavy` ctest (MSVC has no TSan, so the
    // CallScope serialization contract is verified by hammering, not by a race
    // detector). Default 1 keeps the normal-suite run fast.
    long long scale = 1;
    if (const char* e = std::getenv("XINSP2_STRESS_SCALE")) {
        long long v = std::atoll(e);
        if (v > 0) scale = v;
    }
    const long long N = 400000 * scale;
    std::atomic<bool> stop{false};

    std::thread proc([&] {
        for (long long i = 0; i < N; ++i) {
            xi_pack_handle out = adapter.run_pack_door(XI_PACK_NULL);
            if (out != XI_PACK_NULL) xi::pack_v1_iface()->release(out);
        }
        stop.store(true);
    });
    std::thread setter([&] {
        long long k = 0;
        while (!stop.load()) {
            std::string j = "{\"t\":" + std::to_string((int)(k++ & 0x7fffffff)) + "}";
            adapter.set_def(j);
        }
    });
    proc.join();
    setter.join();

    std::string def = adapter.get_def();
    long long tears = 0;
    auto pos = def.find("\"tears\":");
    if (pos != std::string::npos) tears = std::atoll(def.c_str() + pos + 8);
    std::printf("  %-14s tears=%lld   (%s)\n",
                reentrant ? "reentrant" : "non-reentrant", tears, def.c_str());
    return tears;
}

int main() {
    HMODULE dll = LoadLibraryA(RACE_PROBE_DLL);
    if (!dll) {
        std::fprintf(stderr, "load %s failed (err %lu)\n", RACE_PROBE_DLL, GetLastError());
        return 2;
    }

    std::printf("=== Round 1: non-reentrant (host serializes via CallScope cap=1) ===\n");
    long long t1 = run_round(dll, false);
    std::printf("=== Round 2: reentrant, unlimited (no host gate) ===\n");
    long long t2 = run_round(dll, true);

    std::printf("\nRESULT: non-reentrant=%lld torn reads (expect 0); reentrant=%lld (expect >0)\n",
                t1, t2);
    // The contract under test: a non-reentrant plugin NEVER sees a torn config
    // read, with zero protection of its own. (Round 2 just demonstrates the
    // race exists once a plugin opts out of the gate — not asserted, timing.)
    if (t1 != 0) {
        std::printf("FAIL: non-reentrant instance saw %lld torn config reads\n", t1);
        return 1;
    }
    std::printf("PASS: host serializes set_def vs process() for a non-reentrant plugin\n");
    if (t2 == 0)
        std::printf("note: reentrant round caught 0 tears this run (timing) — race still real\n");
    return 0;
}
