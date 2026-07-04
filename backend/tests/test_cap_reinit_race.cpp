//
// test_cap_reinit_race.cpp — A1/A2 regression (red-team hardening,
// docs/new_gen/21-redteam-load-findings.md): the capability funnel runs the
// provider handler under NO CallScope, so on_fault=reinit — which rebuilds and
// DESTROYS the old inst_ — could free inst_ out from under a concurrent cap
// handler (the shared_ptr pin protects the adapter, not inst_). A UAF.
//
// The fix is a per-adapter reader/writer "reinit gate": a cap call takes the
// SHARED side around its handler run (re-resolving the live handler/self under
// it); reinit takes the EXCLUSIVE side around destroy_fn_(old), so the destroy
// drains every in-flight handler first.
//
// This probe reproduces the race: many reader threads hammer test.echo through
// the funnel while a writer thread repeatedly reinit()s the SAME adapter. Under
// the fix it runs clean and stays balanced; under the bug it UAFs (caught by
// ASan/TSan, and often a hard crash even in Release). It also proves the gate
// does not DEADLOCK and that the capability is still live + correct afterward.
//
#include <xi/xi_cap_abi.hpp>
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_instance.hpp>
#include <xi/xi_pack_abi.hpp>
#include <xi/xi_seh.hpp>
#include <xi/xi_abi.h>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#ifndef CAP_TEST_PLUGIN_DLL
#define CAP_TEST_PLUGIN_DLL "cap_test_plugin.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

int main() {
    std::printf("[test] cap reinit race (A1/A2 regression)\n");
    xi::install_seh_translator();

    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();
    xi::install_cap_plane();

    const auto* cap = static_cast<const xi_cap_v1*>(host.get_interface("xi.cap", 1));
    const auto* pk  = static_cast<const xi_pack_v1*>(host.get_interface("xi.pack", 1));
    CHECK(cap && pk);
    if (!cap || !pk) return 1;

    HMODULE dll = LoadLibraryA(CAP_TEST_PLUGIN_DLL);
    if (!dll) {
        std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", CAP_TEST_PLUGIN_DLL, GetLastError());
        return 1;
    }
    auto create = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(dll, "xi_plugin_create"));
    CHECK(create != nullptr);
    if (!create) return 1;

    // Build the provider adapter and ARM reinit (the PM does this at create time).
    xi::ImagePoolOwnerScope scope;
    void* raw = scope.run_factory([&] { return create(&host, "libr"); });
    auto ad = std::make_shared<xi::CAbiInstanceAdapter>(
        "libr", "cap_test_plugin", dll, raw, /*reentrant=*/true, /*max_conc=*/0);
    ad->adopt_owner_id(scope.release());
    ad->arm_reinit(create, &host);
    xi::InstanceRegistry::instance().add(ad);
    CHECK(cap->available("test.echo") == 1);

    const size_t frames_baseline = xi::PackRegistry::instance().live_frames();

    std::atomic<bool> stop{false};
    std::atomic<long> ok_calls{0}, transient{0}, reinits{0};

    // Reader threads: hammer test.echo through the funnel. Any resolved handler
    // must NOT be running against a freed inst_ (the UAF this guards). Transient
    // EUNKNOWN (mid-sweep) / EQUARANTINED are acceptable — not crashes.
    auto reader = [&] {
        while (!stop.load(std::memory_order_relaxed)) {
            xi_pack_builder b = pk->builder_new();
            pk->builder_add_i64(b, "x", 41);
            xi_pack_handle in = pk->builder_seal(b);
            xi_pack_handle out = XI_PACK_NULL;
            int32_t rc = cap->call("test.echo", in, &out);
            if (rc == XI_CAP_OK) {
                int64_t v = -1; if (out != XI_PACK_NULL) pk->get_i64(out, "x_echo", &v);
                CHECK(v == 42);               // the fresh/old handler is always coherent
                ok_calls.fetch_add(1, std::memory_order_relaxed);
            } else {
                transient.fetch_add(1, std::memory_order_relaxed);
            }
            if (out != XI_PACK_NULL) pk->release(out);
            pk->release(in);
        }
    };

    // Writer thread: repeatedly rebuild the SAME adapter in place. Each reinit
    // destroys the old inst_ — the EXCLUSIVE side of the gate must drain in-flight
    // handlers before that destroy.
    auto writer = [&] {
        for (int i = 0; i < 400; ++i) {
            CHECK(ad->reinit());              // must return true (armed) and not crash
            reinits.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    };

    std::vector<std::thread> pool;
    for (int i = 0; i < 6; ++i) pool.emplace_back(reader);
    std::thread w(writer);
    w.join();
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : pool) t.join();

    std::printf("  reinits=%ld ok_calls=%ld transient=%ld\n",
                reinits.load(), ok_calls.load(), transient.load());
    CHECK(reinits.load() == 400);
    CHECK(ok_calls.load() > 0);               // the funnel kept serving across rebuilds

    // Live + correct after the storm.
    CHECK(cap->available("test.echo") == 1);
    {
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", 100);
        xi_pack_handle in = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("test.echo", in, &out) == XI_CAP_OK);
        int64_t v = -1; if (out != XI_PACK_NULL) pk->get_i64(out, "x_echo", &v);
        CHECK(v == 101);
        if (out != XI_PACK_NULL) pk->release(out);
        pk->release(in);
    }

    xi::InstanceRegistry::instance().remove("libr");
    ad.reset();
    CHECK(xi::PackRegistry::instance().live_frames() == frames_baseline);

    FreeLibrary(dll);
    if (g_failures == 0) { std::printf("\n[test] cap reinit race: ALL OK\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
