//
// test_cap_reload_window.cpp — A3 + B2 triage
// (docs/new_gen/22-plausible-triage.md).
//
// A3 (spurious -4 / XI_CAP_ESHAPE) and B2 (unavailability window / -1 /
// XI_CAP_EUNKNOWN) both concern the capability funnel during a provider swap
// (PluginManager::reload_machine_provider -> evict_machine_provider_locked_ then
// autoload re-creates). The evict path is:
//
//     InstanceRegistry::instance().remove(name);   // (1) out of the live list
//     machine_instances_.erase(it);                 // (2) dtor -> sweep_caps_for
//     ... autoload ...                              // (3) fresh factory re-registers
//
// A concurrent f_cap_call takes NO PluginManager lock, so it can land:
//   * between (1) and (2): CapRegistry still has the name, but resolve_provider_
//     no longer finds the adapter -> XI_CAP_ESHAPE (-4)  [A3]
//   * between (2) and (3): the cap name is swept, lookup misses -> XI_CAP_EUNKNOWN
//     (-1)                                                [B2]
//
// This probe reproduces the swap under concurrent load: reader threads hammer
// test.echo while a writer repeatedly evicts + re-creates the provider (a fresh
// owner id each time, exactly as reload does). It MEASURES the -4/-1 transient
// rate and asserts the important safety properties: the funnel never crashes,
// never returns a WRONG answer (a resolved handler is always coherent — the
// RT6 reinit-gate guarantees no freed inst_ is entered), the pack table
// balances, and the capability resolves correctly again after the storm.
//
// VERDICT (see doc 22): the windows are REAL (this test observes non-zero
// transients) but BENIGN — fail-soft error codes on a caller-retry contract, no
// memory-safety hazard. So this stays a GREEN characterization test (it asserts
// survival + eventual correctness + coherence, NOT zero transients).
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
#include <mutex>
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
    std::printf("[test] cap reload/swap availability window (A3/B2 triage)\n");
    xi::install_seh_translator();

    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();
    xi::install_cap_plane();

    const auto* cap = static_cast<const xi_cap_v1*>(host.get_interface("xi.cap", 1));
    const auto* pk  = static_cast<const xi_pack_v1*>(host.get_interface("xi.pack", 1));
    CHECK(cap && pk);
    if (!cap || !pk) return 2;

    HMODULE dll = LoadLibraryA(CAP_TEST_PLUGIN_DLL);
    if (!dll) {
        std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", CAP_TEST_PLUGIN_DLL, GetLastError());
        return 2;
    }
    auto create = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(dll, "xi_plugin_create"));
    CHECK(create != nullptr);
    if (!create) return 2;

    // Mirror the PM create path: factory under an owner scope, adapter adopts it,
    // added to the live registry (so the funnel's resolve_provider_ finds it).
    auto make_lib = [&]() -> std::shared_ptr<xi::CAbiInstanceAdapter> {
        xi::ImagePoolOwnerScope scope;
        void* raw = scope.run_factory([&] { return create(&host, "swaplib"); });
        auto ad = std::make_shared<xi::CAbiInstanceAdapter>(
            "swaplib", "cap_test_plugin", dll, raw, /*reentrant=*/true, /*max_conc=*/0);
        ad->adopt_owner_id(scope.release());
        xi::InstanceRegistry::instance().add(ad);
        return ad;
    };

    // The live provider, guarded so a mid-swap teardown never races the reader's
    // shared_ptr resolve at the C++ level (the funnel pins via resolve_provider_).
    std::mutex live_mu;
    std::shared_ptr<xi::CAbiInstanceAdapter> live = make_lib();
    CHECK(cap->available("test.echo") == 1);

    const size_t frames_baseline = xi::PackRegistry::instance().live_frames();

    std::atomic<bool> stop{false};
    std::atomic<long> ok{0}, eunknown{0}, eshape{0}, equar{0}, other{0}, wrong{0};

    auto reader = [&] {
        while (!stop.load(std::memory_order_relaxed)) {
            xi_pack_builder b = pk->builder_new();
            pk->builder_add_i64(b, "x", 41);
            xi_pack_handle in = pk->builder_seal(b);
            xi_pack_handle out = XI_PACK_NULL;
            int32_t rc = cap->call("test.echo", in, &out);
            if (rc == XI_CAP_OK) {
                int64_t v = -1; if (out != XI_PACK_NULL) pk->get_i64(out, "x_echo", &v);
                if (v != 42) wrong.fetch_add(1, std::memory_order_relaxed); // must never happen
                ok.fetch_add(1, std::memory_order_relaxed);
            } else if (rc == XI_CAP_EUNKNOWN)     eunknown.fetch_add(1, std::memory_order_relaxed);
            else if (rc == XI_CAP_ESHAPE)         eshape.fetch_add(1, std::memory_order_relaxed);
            else if (rc == XI_CAP_EQUARANTINED)   equar.fetch_add(1, std::memory_order_relaxed);
            else                                  other.fetch_add(1, std::memory_order_relaxed);
            if (out != XI_PACK_NULL) pk->release(out);
            pk->release(in);
        }
    };

    // Writer: evict + re-create the provider, the reload_machine_provider shape.
    // remove-from-registry FIRST (matches evict_machine_provider_locked_), drop
    // the ref (dtor sweeps caps), then stand a fresh instance up.
    const int kSwaps = 300;
    std::atomic<long> swaps{0};
    auto writer = [&] {
        for (int i = 0; i < kSwaps && !stop.load(std::memory_order_relaxed); ++i) {
            std::shared_ptr<xi::CAbiInstanceAdapter> old;
            {
                std::lock_guard<std::mutex> lk(live_mu);
                old = live;
                live.reset();
            }
            xi::InstanceRegistry::instance().remove("swaplib");  // (1)
            old.reset();                                          // (2) dtor sweeps caps
            std::this_thread::yield();
            auto fresh = make_lib();                              // (3) re-register
            {
                std::lock_guard<std::mutex> lk(live_mu);
                live = fresh;
            }
            swaps.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    };

    std::vector<std::thread> pool;
    for (int i = 0; i < 6; ++i) pool.emplace_back(reader);
    std::thread w(writer);
    w.join();
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : pool) t.join();

    std::printf("  swaps=%ld ok=%ld  transient: EUNKNOWN(-1)=%ld ESHAPE(-4)=%ld "
                "EQUAR(-3)=%ld other=%ld  wrong_answers=%ld\n",
                swaps.load(), ok.load(), eunknown.load(), eshape.load(),
                equar.load(), other.load(), wrong.load());

    // Safety assertions (the point of the test):
    CHECK(swaps.load() == kSwaps);          // the writer completed the swap storm
    CHECK(ok.load() > 0);                    // the funnel kept serving across swaps
    CHECK(wrong.load() == 0);                // NEVER a wrong answer (no freed-inst_ call)
    CHECK(other.load() == 0);                // only the documented fail-soft codes appear

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

    xi::InstanceRegistry::instance().remove("swaplib");
    { std::lock_guard<std::mutex> lk(live_mu); live.reset(); }
    CHECK(xi::PackRegistry::instance().live_frames() == frames_baseline);

    FreeLibrary(dll);
    if (g_failures == 0) { std::printf("\n[test] cap reload window: ALL OK\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
