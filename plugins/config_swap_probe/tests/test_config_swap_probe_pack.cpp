//
// test_config_swap_probe_pack.cpp — the config_swap_probe pack-door proof
// (docs/new_gen/10 gate P1). v12: the Record process path is deleted, so the
// xi.pack@1 door is the SOLE data plane; this test drives that door directly.
//
// config_swap_probe's process() is a no-op OBSERVATION probe: it reads the live
// slot into last_seen_ and bumps a call counter, returning an EMPTY pack. Its
// real surface is the config/prepare/commit control plane, observed through the
// get_status Status extractor. So coverage drives the door N times and checks
// the OBSERVABLE STATE it leaves behind — field for field through Status.
//
// Loads the REAL built DLL through the genuine C-ABI + host adapter (like
// pack_pilot_test.cpp) so the pack door runs exactly as the backend drives it.
// Checks:
//   (A) PLUGIN-door probe — config_swap_probe publishes xi_plugin_get_interface
//       and answers ("xi.pack", 1); the adapter reflects this via has_pack_door().
//   (B) The door drives the observation probe: N drives -> proc == N,
//       last_seen == the live value; the empty result pack carries no entries
//       and no $fault.
//   (C) UNKNOWN-ENTRY tolerance — a pack carrying junk keys drives the door fine
//       (no input contract, no fault); it's a probe.
//   (D) TWO-PHASE observability — prepare() stages without touching the live
//       slot, commit() swaps; observed through get_status, interleaved with
//       pack-door drives.
//   (E) Pooled-handle + pack-registry balance across the whole test.
//
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include <xi/xi_cabi_adapter.hpp>   // CAbiInstanceAdapter (has_pack_door / run_pack_door)
#include <xi/xi_image_pool.hpp>     // make_host_api + cumulative().live_now
#include <xi/xi_pack_abi.hpp>       // install_pack_abi + pack_v1_iface + PackRegistry

#include "config_swap_probe_io.gen.h"   // Config / Command / Status typed views

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifndef CONFIG_SWAP_PROBE_DLL_PATH
#define CONFIG_SWAP_PROBE_DLL_PATH "xi-config_swap_probe.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

namespace csp = xi::config_swap_probe;

static int pool_live() { return xi::ImagePool::instance().cumulative().live_now; }

struct LoadedPlugin {
    HMODULE dll = nullptr;
    void*   inst = nullptr;
    std::unique_ptr<xi::CAbiInstanceAdapter> adapter;
    xi_plugin_get_interface_fn get_iface = nullptr;
};

static LoadedPlugin load_plugin(const char* path, const char* name,
                                const xi_host_api* host) {
    LoadedPlugin lp;
    lp.dll = LoadLibraryA(path);
    if (!lp.dll) { std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", path, GetLastError()); ++g_failures; return lp; }
    auto factory = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(lp.dll, "xi_plugin_create"));
    CHECK(factory != nullptr);
    if (factory) lp.inst = factory(host, name);
    CHECK(lp.inst != nullptr);
    lp.get_iface = reinterpret_cast<xi_plugin_get_interface_fn>(GetProcAddress(lp.dll, "xi_plugin_get_interface"));
    if (lp.inst)
        lp.adapter = std::make_unique<xi::CAbiInstanceAdapter>(name, name, lp.dll, lp.inst);
    return lp;
}

// Drive the pack door once with the given input pack; returns the door's output
// pack handle (caller releases). The input pack is built + released here.
static xi_pack_handle drive_pack(const xi_pack_v1* fi, xi::CAbiInstanceAdapter& a,
                                 bool with_junk) {
    xi_pack_builder b = fi->builder_new();
    if (with_junk) {                       // a probe ignores unknown entries
        fi->builder_add_i64(b, "junk_key", 12345);
        fi->builder_add_str(b, "note", "ignored", 7);
    }
    xi_pack_handle in = fi->builder_seal(b);
    xi_pack_handle out = a.run_pack_door(in);
    fi->release(in);
    return out;
}

static csp::Status status(xi::CAbiInstanceAdapter& a) {
    return csp::Status{ a.exchange(csp::Command::get_status()) };
}

int main() {
    std::printf("[test] config_swap_probe xi.pack@1 door (v12: pack is the sole data plane)\n");

    xi::install_pack_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    const xi_pack_v1* fi = xi::pack_v1_iface();
    int base = pool_live();

    LoadedPlugin pack = load_plugin(CONFIG_SWAP_PROBE_DLL_PATH, "probe_pack", &host);
    if (!pack.inst) { std::fprintf(stderr, "\nSETUP FAILED\n"); return 1; }

    // ---------------------------------------------------------------------
    // (A) The probe publishes the xi.pack@1 capability door.
    // ---------------------------------------------------------------------
    SECTION("plugin door probe: config_swap_probe answers xi.pack@1");
    CHECK(pack.get_iface != nullptr);
    if (pack.get_iface) {
        CHECK(pack.get_iface("xi.pack", 1) != nullptr);
        CHECK(pack.get_iface("xi.pack", 2) == nullptr);   // only @1 published
        CHECK(pack.get_iface("xi.other", 1) == nullptr);
    }
    CHECK(pack.adapter->has_pack_door());

    // ---------------------------------------------------------------------
    // (B) The door drives the observation probe: N drives -> proc == N, and the
    //     live slot value is observed into last_seen. The empty result pack
    //     carries no entries and no $fault.
    // ---------------------------------------------------------------------
    SECTION("pack door: N drives observe the live slot and bump the counter");
    {
        std::string cfg = csp::Config().value(42);
        CHECK(pack.adapter->set_def(cfg));

        const int N = 3;
        for (int i = 0; i < N; ++i) {
            xi_pack_handle out = drive_pack(fi, *pack.adapter, /*with_junk=*/false);
            CHECK(out != XI_PACK_NULL);          // a real (empty) result pack, not the hard-fail sentinel
            if (out) {
                CHECK(fi->count(out) == 0);      // no output data — the probe emits nothing
                const char* fc = nullptr; int32_t fl = 0;
                CHECK(fi->get_str(out, "$fault", &fc, &fl) == 0);   // no input contract -> never a fault
                fi->release(out);
            }
        }

        auto ps = status(*pack.adapter);
        CHECK(ps.active() == 42);
        CHECK(ps.last_seen() == 42);
        CHECK(ps.proc() == N);
    }

    // ---------------------------------------------------------------------
    // (C) A probe ignores unknown pack entries — no input contract, no fault.
    // ---------------------------------------------------------------------
    SECTION("pack door: unknown entries ignored (it's a probe, no required inputs)");
    {
        xi_pack_handle out = drive_pack(fi, *pack.adapter, /*with_junk=*/true);
        CHECK(out != XI_PACK_NULL);
        if (out) {
            const char* fc = nullptr; int32_t fl = 0;
            CHECK(fi->get_str(out, "$fault", &fc, &fl) == 0);   // junk in, no fault out
            CHECK(fi->count(out) == 0);
            fi->release(out);
        }
        // The junk drive still observed the live slot + bumped the counter.
        auto ps = status(*pack.adapter);
        CHECK(ps.active() == 42);
        CHECK(ps.last_seen() == 42);
        CHECK(ps.proc() == 4);               // 3 clean + 1 junk drive
    }

    // ---------------------------------------------------------------------
    // (D) Two-phase prepare/commit observability, interleaved with pack drives.
    // ---------------------------------------------------------------------
    SECTION("two-phase prepare/commit observability");
    {
        // prepare stages WITHOUT touching the live slot.
        CHECK(pack.adapter->prepare(csp::Config().value(99), ""));
        {
            auto s = status(*pack.adapter);
            CHECK(s.active() == 42);          // live still old
            CHECK(s.has_staged());
            CHECK(s.staged_value() == 99);
        }
        // A pack-door drive between prepare and commit still observes the OLD
        // live slot (staging is invisible to process()).
        {
            xi_pack_handle out = drive_pack(fi, *pack.adapter, false);
            if (out) fi->release(out);
            auto s = status(*pack.adapter);
            CHECK(s.last_seen() == 42);       // saw live, not staged
            CHECK(s.has_staged());            // commit hasn't run
        }
        // commit swaps staging -> live atomically.
        pack.adapter->commit();
        {
            auto s = status(*pack.adapter);
            CHECK(s.active() == 99);
            CHECK(!s.has_staged());
        }
        // A drive after commit now observes the NEW live slot through the door.
        {
            xi_pack_handle out = drive_pack(fi, *pack.adapter, false);
            if (out) fi->release(out);
            auto s = status(*pack.adapter);
            CHECK(s.last_seen() == 99);
        }
    }

    // ---------------------------------------------------------------------
    // (E) Teardown + pooled-handle / pack-registry balance.
    // ---------------------------------------------------------------------
    pack.adapter.reset();
    FreeLibrary(pack.dll);

    CHECK(pool_live() == base);
    CHECK(xi::PackRegistry::instance().live_frames() == 0);

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
