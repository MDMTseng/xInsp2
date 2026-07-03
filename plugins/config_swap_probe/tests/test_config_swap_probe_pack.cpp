//
// test_config_swap_probe_pack.cpp — the polaris2 wave-2 (docs/new_gen/10 gate
// P1) bilingual proof for config_swap_probe: the plugin now speaks BOTH the
// Record currency AND the xi.pack@1 pack-in/pack-out door, and the two agree.
//
// config_swap_probe's process() is a no-op OBSERVATION probe: it reads the live
// slot into last_seen_ and bumps a call counter, returning empty. Its real
// surface is the config/prepare/commit control plane, observed through the
// get_status Status extractor. So the "does the pack door mirror the Record
// path?" question is answered NOT by comparing the (empty) process return, but
// by driving each path an equal number of times and comparing the OBSERVABLE
// STATE they leave behind — field for field through the Status extractor.
//
// Loads the REAL built DLL through the genuine C-ABI + host adapter (like
// pack_pilot_test.cpp) so the pack door runs exactly as the backend drives it.
// Checks:
//
//   (A) PLUGIN-door probe — config_swap_probe now publishes
//       xi_plugin_get_interface and answers ("xi.pack", 1); the adapter
//       reflects this via has_pack_door().
//   (B) RECORD vs PACK parity — two fresh instances configured identically;
//       one driven N times through the Record process(), the other N times
//       through the pack door; get_status is identical field-for-field
//       (active / staged / staged_value / last_seen / proc). The empty pack the
//       door returns carries no output entries and no $fault (mirrors `{}`).
//   (C) UNKNOWN-ENTRY tolerance — a pack carrying junk keys drives the door
//       fine (no input contract, no fault); it's a probe.
//   (D) TWO-PHASE observability unchanged — prepare() stages without touching
//       the live slot, commit() swaps; observed through get_status exactly as
//       on the Record-only build, now interleaved with pack-door drives.
//   (E) Pooled-handle + pack-registry balance across the whole test.
//
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include <xi/xi_cabi_adapter.hpp>   // CAbiInstanceAdapter (has_pack_door / run_pack_door / process)
#include <xi/xi_image_pool.hpp>     // make_host_api + cumulative().live_now
#include <xi/xi_pack_abi.hpp>       // install_pack_abi + pack_v1_iface + PackRegistry

#include "config_swap_probe_io.gen.h"   // Config / Command / Status typed views

#include <cstdint>
#include <cstdio>
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

// Drive the Record no-op process() once (empty input, empty output).
static void drive_record(xi::CAbiInstanceAdapter& a) {
    std::string empty = "{}";
    xi_record in{};
    in.data = reinterpret_cast<const uint8_t*>(empty.c_str());
    in.len  = 2;
    xi_record_out out; xi_record_out_init(&out);
    a.process(&in, &out);
    xi_record_out_free(&out);
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

// Field-for-field equality of the observable state the two currencies leave.
static void expect_status_equal(const csp::Status& r, const csp::Status& p) {
    CHECK(r.valid() && p.valid());
    CHECK(r.active()       == p.active());
    CHECK(r.has_staged()   == p.has_staged());
    CHECK(r.staged_value() == p.staged_value());
    CHECK(r.last_seen()    == p.last_seen());
    CHECK(r.proc()         == p.proc());
}

int main() {
    std::printf("[test] config_swap_probe bilingual: Record path vs xi.pack@1 door parity\n");

    xi::install_pack_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    const xi_pack_v1* fi = xi::pack_v1_iface();
    int base = pool_live();

    // rec-driven instance and pack-driven instance — same DLL, independent state.
    LoadedPlugin rec  = load_plugin(CONFIG_SWAP_PROBE_DLL_PATH, "probe_rec",  &host);
    LoadedPlugin pack = load_plugin(CONFIG_SWAP_PROBE_DLL_PATH, "probe_pack", &host);
    if (!rec.inst || !pack.inst) { std::fprintf(stderr, "\nSETUP FAILED\n"); return 1; }

    // ---------------------------------------------------------------------
    // (A) The probe now publishes the xi.pack@1 capability door.
    // ---------------------------------------------------------------------
    SECTION("plugin door probe: config_swap_probe answers xi.pack@1");
    CHECK(pack.get_iface != nullptr);
    if (pack.get_iface) {
        CHECK(pack.get_iface("xi.pack", 1) != nullptr);
        CHECK(pack.get_iface("xi.pack", 2) == nullptr);   // only @1 published
        CHECK(pack.get_iface("xi.other", 1) == nullptr);
    }
    CHECK(pack.adapter->has_pack_door());
    CHECK(rec.adapter->has_pack_door());   // same DLL — both instances speak packs

    // ---------------------------------------------------------------------
    // (B) Record vs pack parity: configure both to 42, drive each path the
    //     SAME number of times, compare the observable state field-for-field.
    // ---------------------------------------------------------------------
    SECTION("record path vs pack door: identical observable state after equal drives");
    {
        std::string cfg = csp::Config().value(42);
        CHECK(rec.adapter->set_def(cfg));
        CHECK(pack.adapter->set_def(cfg));

        const int N = 3;
        for (int i = 0; i < N; ++i) drive_record(*rec.adapter);
        for (int i = 0; i < N; ++i) {
            xi_pack_handle out = drive_pack(fi, *pack.adapter, /*with_junk=*/false);
            CHECK(out != XI_PACK_NULL);          // a real (empty) result pack, not the hard-fail sentinel
            if (out) {
                CHECK(fi->count(out) == 0);      // mirrors the Record path's empty `{}`
                const char* fc = nullptr; int32_t fl = 0;
                CHECK(fi->get_str(out, "$fault", &fc, &fl) == 0);   // no input contract -> never a fault
                fi->release(out);
            }
        }

        auto rs = status(*rec.adapter);
        auto ps = status(*pack.adapter);
        CHECK(rs.active() == 42 && ps.active() == 42);
        CHECK(rs.last_seen() == 42);
        CHECK(rs.proc() == N);
        expect_status_equal(rs, ps);            // the two currencies agree exactly
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
    // (D) Two-phase prepare/commit observability is unchanged by the door,
    //     interleaved here with a pack-door drive on the pack-driven instance.
    // ---------------------------------------------------------------------
    SECTION("two-phase prepare/commit observability unchanged");
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
    rec.adapter.reset();
    pack.adapter.reset();
    FreeLibrary(rec.dll);
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
