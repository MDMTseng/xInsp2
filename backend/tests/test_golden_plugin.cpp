//
// test_golden_plugin.cpp — the golden-plugin ABI compatibility test (Phase 0).
//
// THE single most important ABI safety net (see docs/internals/adr-001-host-api-
// freeze.md). It loads the GOLDEN plugin (golden_plugin.cpp, re-pinned to the
// current ABI v11 / min-compat 11) through the REAL plugin-load path and runs
// process() once, asserting byte-for-byte stable behaviour. Run it on every ABI
// change: if a move breaks how a valid plugin loads or how process() sees the
// host, this breaks first.
//
// Phase 4 addition: it ALSO loads two DELIBERATELY STALE fixtures (stale_plugin,
// reporting v9 and v10 — the old shm-bearing layout) and asserts the raised
// min-compat gate now REFUSES them. That proves the authorized major break
// (core_fix_plan.md §12 Phase 4) actually bites: a pre-v11 plugin can no longer
// load against the reshuffled v11 table.
//
// Path exercised, end to end:
//   1. plugin_abi_compatible(dll, …)      — the real ABI-version + yyjson gate.
//   2. xi_plugin_create(make_host_api(), …) — a real host_api backed by ImagePool.
//   3. CAbiInstanceAdapter::process(…)     — the real per-instance adapter
//      (OwnerGuard + CallScope), exactly as the dispatch worker drives it.
//   4. assert the produced image's geometry + deterministic pixel ramp.
//
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_image_pool.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdint>
#include <cstdio>

#ifndef GOLDEN_PLUGIN_DLL
#define GOLDEN_PLUGIN_DLL "golden_plugin.dll"
#endif
#ifndef STALE_V9_PLUGIN_DLL
#define STALE_V9_PLUGIN_DLL "stale_plugin_v9.dll"
#endif
#ifndef STALE_V10_PLUGIN_DLL
#define STALE_V10_PLUGIN_DLL "stale_plugin_v10.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Load a deliberately-stale (pre-v11) plugin DLL and assert the real ABI gate
// REFUSES it — the raised min-compat floor (Phase 4) turning away an old
// shm-bearing-layout plugin. Returns nothing; bumps g_failures on any surprise.
static void expect_stale_refused(const char* dll_path, int expect_reported_ver) {
    HMODULE sdll = LoadLibraryA(dll_path);
    if (!sdll) {
        std::fprintf(stderr, "FAIL: LoadLibrary(%s) failed (err %lu)\n",
                     dll_path, GetLastError());
        ++g_failures;
        return;
    }
    auto sver = reinterpret_cast<int (*)()>(GetProcAddress(sdll, "xi_plugin_abi_version"));
    CHECK(sver != nullptr);
    if (sver) CHECK(sver() == expect_reported_ver);   // genuinely reports the stale version

    // The REAL gate the loader uses. A pre-v11 plugin must be refused with a
    // reason naming the min-compat floor.
    std::string gate_err;
    bool ok = xi::plugin_abi_compatible(sdll, "stale", /*json_fallback_opt_in=*/false,
                                        &gate_err);
    CHECK(!ok);                       // REFUSED — the break bites
    CHECK(!gate_err.empty());         // and it says why
    std::printf("  stale v%d plugin refused as expected: %s\n",
                expect_reported_ver, gate_err.c_str());
    FreeLibrary(sdll);
}

int main() {
    std::printf("[test] golden-plugin ABI v%d compat — load + process() through "
                "the real adapter\n", XI_ABI_VERSION);

    // The golden contract this test guards. Post Phase 4 the golden plugin is a
    // CURRENT (v11 / min-compat 11) binary (golden_plugin.cpp pins
    // kGoldenAbiVersion = XI_ABI_MIN_COMPAT); this test proves a current plugin
    // still loads + runs, and (below) that a stale pre-v11 plugin is refused.
    // Pinned here so a careless ABI edit fails the build, not just the run.
    static_assert(XI_ABI_VERSION == 11, "Phase 4 baseline: host is v11");
    static_assert(XI_ABI_MIN_COMPAT == 11, "Phase 4 raised min-compat to 11");

    HMODULE dll = LoadLibraryA(GOLDEN_PLUGIN_DLL);
    if (!dll) {
        std::fprintf(stderr, "FAIL: LoadLibrary(%s) failed (err %lu)\n",
                     GOLDEN_PLUGIN_DLL, GetLastError());
        return 2;
    }

    // (0) The golden is a CURRENT (v11) plugin sitting exactly at the min-compat
    // floor — the oldest still-loadable version. It proves a current plugin loads;
    // the stale fixtures below prove pre-v11 plugins are refused.
    auto abi_ver = reinterpret_cast<int (*)()>(
        GetProcAddress(dll, "xi_plugin_abi_version"));
    CHECK(abi_ver != nullptr);
    if (abi_ver) {
        int gv = abi_ver();
        std::printf("  golden reports ABI v%d; host is v%d (min-compat %d)\n",
                    gv, XI_ABI_VERSION, XI_ABI_MIN_COMPAT);
        CHECK(gv == XI_ABI_MIN_COMPAT); // pinned to the floor (v11)
        CHECK(gv <= XI_ABI_VERSION);    // loadable on this host
    }

    // (1) Real ABI gate — the same call the loader makes. A golden plugin that
    // no longer passes the gate is an ABI break.
    std::string gate_err;
    CHECK(xi::plugin_abi_compatible(dll, "golden", /*json_fallback_opt_in=*/false,
                                    &gate_err));
    if (!gate_err.empty())
        std::fprintf(stderr, "  gate error: %s\n", gate_err.c_str());

    auto create = reinterpret_cast<void* (*)(const xi_host_api*, const char*)>(
        GetProcAddress(dll, "xi_plugin_create"));
    CHECK(create != nullptr);
    if (!create) { FreeLibrary(dll); return 1; }

    // (2) A real host_api backed by the live ImagePool — exactly what the backend
    // hands a plugin at create().
    static xi_host_api host = xi::ImagePool::make_host_api();
    CHECK(host.get_interface != nullptr);          // query door present on the host
    // Phase 4: xi.legacy@9 is RETIRED — the whole-table view is no longer
    // published, so the door now returns null for it (carved interfaces remain).
    CHECK(host.get_interface("xi.legacy", 9) == nullptr);
    CHECK(host.get_interface("xi.imaging", 1) != nullptr);   // carved capability still there
    void* inst = create(&host, "golden0");
    CHECK(inst != nullptr);

    // (3) Drive through the real adapter (OwnerGuard + CallScope). Non-reentrant.
    {
        xi::CAbiInstanceAdapter adapter("golden0", "golden", dll, inst,
                                        /*reentrant=*/false, /*max_concurrency=*/0);

        xi_record in{};
        xi_record_out out;
        xi_record_out_init(&out);
        int n = adapter.process(&in, &out);

        // (4) Frozen behaviour: one image "out", 4x4x1, pixel[i] == i*16.
        CHECK(n == 1);
        CHECK(out.image_count == 1);
        if (out.image_count == 1) {
            CHECK(std::string(out.images[0].key) == "out");
            xi_image_handle h = out.images[0].handle;
            CHECK(h != XI_IMAGE_NULL);
            CHECK(host.image_width(h) == 4);
            CHECK(host.image_height(h) == 4);
            CHECK(host.image_channels(h) == 1);
            uint8_t* px = host.image_data(h);
            int32_t  stride = host.image_stride(h);
            CHECK(px != nullptr);
            if (px) {
                bool ramp_ok = true;
                for (int32_t y = 0; y < 4; ++y)
                    for (int32_t x = 0; x < 4; ++x) {
                        int32_t i = y * 4 + x;
                        if (px[y * stride + x] != (uint8_t)((i * 16) & 0xFF))
                            ramp_ok = false;
                    }
                CHECK(ramp_ok);
                // A couple of explicit anchors so a failure prints something useful.
                CHECK(px[0] == 0);
                CHECK(px[1 * stride + 1] == 80);    // i=5  -> 80
                CHECK(px[3 * stride + 3] == 240);   // i=15 -> 240
            }
            // The adapter took ownership tag of this handle; release our view of
            // it so the pool count returns to baseline. (out_doc unused here.)
            host.image_release(h);
        }
        xi_record_out_free(&out);
        // adapter dtor calls xi_plugin_destroy + sweeps the owner bucket.
    }

    FreeLibrary(dll);

    // (5) Phase 4 — prove the break BITES: stale pre-v11 plugins (v9 and v10,
    // the old shm-bearing layout) are now REFUSED by the raised min-compat gate.
    std::printf("[test] Phase-4 break — stale pre-v11 plugins must be REFUSED\n");
    expect_stale_refused(STALE_V9_PLUGIN_DLL, 9);
    expect_stale_refused(STALE_V10_PLUGIN_DLL, 10);

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
