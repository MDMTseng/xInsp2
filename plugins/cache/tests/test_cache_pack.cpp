//
// test_cache_pack.cpp — the cache (BufferReplay) plugin's xi.pack@1 side, end to
// end against the REAL built DLL (polaris2 wave-2, docs/new_gen/10 gate P1).
//
// cache is the most retention-heavy of the bilingual batch: it BUFFERS what
// flows past and REPLAYS it later. On the pack path that means RETAINING a
// sealed host Pack beyond the frame that produced it, so this test exercises the
// pack retain/release ABI for real, with two independent leak oracles:
//   * xi::PackRegistry::live_frames() — sealed packs the host still holds.
//   * ImagePool::cumulative().live_now — pooled image buffers (each pack here
//     carries one image ⇒ one pool handle).
//
// Coverage:
//   (A) capture N packs through the door → ring stats + both oracles climb by N.
//   (B) eviction (capacity shrink of the ring) RELEASES the evicted pack's ref —
//       both oracles fall; nothing dangles.
//   (C) replay re-emits the SAME sealed handle with a FRESH trigger id and ZERO
//       pixel copy: the replayed event's handle EQUALS the buffered handle, the
//       pool live-count is unchanged across replay, and the image bytes match.
//   (D) mixed ring — records and packs interleaved replay in capture order.
//   (E) teardown (destroy) releases every retained pack → both oracles return to
//       baseline.
//
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include <xi/xi_cabi_adapter.hpp>   // CAbiInstanceAdapter (has_pack_door / run_pack_door / process)
#include <xi/xi_image_pool.hpp>     // make_host_api + cumulative().live_now
#include <xi/xi_pack_abi.hpp>      // install_pack_abi + pack_v1_iface + PackRegistry
#include <xi/xi_trigger_bus.hpp>    // install_trigger_hook + the dispatch sink
#include <xi/xi_record.hpp>

#include "cache_io.h"               // the typed control/status view (Status/Command/Config)

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef CACHE_DLL_PATH
#define CACHE_DLL_PATH "xi-cache.dll"
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

namespace ck = xi::cache::keys;

static const xi_pack_v1* g_fi = nullptr;
static xi_host_api        g_host;

static int    pool_live()   { return xi::ImagePool::instance().cumulative().live_now; }
static size_t live_frames() { return xi::PackRegistry::instance().live_frames(); }

// A captured dispatch event (record OR pack), recorded by the bus sink.
struct Captured {
    xi_pack_handle pack = XI_PACK_NULL;   // XI_PACK_NULL ⇒ a Record event
    xi_trigger_id  id   = XI_TRIGGER_NULL;
};
static std::mutex           g_mu;
static std::vector<Captured> g_events;

// Build a sealed pack carrying one WxH grayscale image (a pool buffer — every
// pack image is pooled, so it shows up in the pool oracle) filled with `fill`,
// plus a "seq" marker so a replayed pack is identifiable in capture order.
static xi_pack_handle make_pack(int seq, uint8_t fill, int W = 64, int H = 64) {
    std::vector<uint8_t> px((size_t)W * H, fill);
    xi_pack_builder b = g_fi->builder_new();
    g_fi->builder_add_image(b, "img", W, H, 1, px.data());
    g_fi->builder_add_i64(b, "seq", seq);
    return g_fi->builder_seal(b);
}

// Capture `in` through the door; assert the ack pack carries {buffered: expect};
// the caller's ref on `in` is released here (the door took its own ring ref).
static void capture_pack(xi::CAbiInstanceAdapter& a, xi_pack_handle in, int expect) {
    xi_pack_handle ack = a.run_pack_door(in);
    CHECK(ack != XI_PACK_NULL);
    if (ack) {
        int64_t buffered = -1;
        CHECK(g_fi->get_i64(ack, ck::kBuffered, &buffered) == 1 && (int)buffered == expect);
        g_fi->release(ack);
    }
    g_fi->release(in);   // drop the producer's ref; the ring keeps its own
}

// Capture a Record (one pool image) through the Record door — for the mixed-ring
// test. Returns nothing; the ring buffers a deep copy.
static void capture_record(xi::CAbiInstanceAdapter& a) {
    xi_image_handle h = g_host.image_create(64, 64, 1);
    std::memset(g_host.image_data(h), 123, 64 * 64);
    xi_record_image imgs[1] = { { "img", h } };
    std::string meta = "{}";
    xi_record in{};
    in.images = imgs; in.image_count = 1;
    in.data = reinterpret_cast<const uint8_t*>(meta.c_str());
    in.len  = (int32_t)meta.size();
    xi_record_out out; xi_record_out_init(&out);
    a.process(&in, &out);
    xi_record_out_free(&out);
    g_host.image_release(h);
}

static xi::cache::Status status(xi::CAbiInstanceAdapter& a) {
    return xi::cache::Status{ a.get_def() };
}

// Drain every event the sink collected, releasing each pack event's ref (the
// dispatcher's job — a real host releases the event pack after the sink runs).
static void drain_events() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& e : g_events)
        if (e.pack != XI_PACK_NULL) xi::TriggerBus::instance().release_pack_(e.pack);
    g_events.clear();
}

int main() {
    std::printf("[test] cache (BufferReplay) xi.pack@1 retention/replay/teardown\n");

    xi::install_pack_abi();
    g_host = xi::ImagePool::make_host_api();
    xi::install_trigger_hook(g_host);
    g_fi = xi::pack_v1_iface();

    // Sink: record every dispatched event (record + pack) in fire order.
    xi::TriggerBus::instance().set_sink([](xi::TriggerEvent ev) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_events.push_back(Captured{ ev.pack, ev.id });
    });

    const int    base_pool   = pool_live();
    const size_t base_frames = live_frames();

    HMODULE dll = LoadLibraryA(CACHE_DLL_PATH);
    if (!dll) { std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", CACHE_DLL_PATH, GetLastError()); return 1; }
    auto factory = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(dll, "xi_plugin_create"));
    CHECK(factory != nullptr);
    void* inst = factory ? factory(&g_host, "buffer") : nullptr;
    CHECK(inst != nullptr);
    if (!inst) return 1;
    auto adapter = std::make_unique<xi::CAbiInstanceAdapter>("buffer", "buffer", dll, inst);

    // The plugin publishes the pack door (bilingual).
    CHECK(adapter->has_pack_door());

    // ---------------------------------------------------------------------
    // (A) Capture N packs → ring stats + both oracles climb by N.
    // ---------------------------------------------------------------------
    SECTION("capture 3 packs -> count/packs == 3, live_frames + pool climb by 3");
    CHECK(adapter->set_def(xi::cache::Config().capacity(8)));
    capture_pack(*adapter, make_pack(0, 10), 1);
    capture_pack(*adapter, make_pack(1, 20), 2);
    capture_pack(*adapter, make_pack(2, 30), 3);
    {
        auto st = status(*adapter);
        CHECK(st.count() == 3);
        CHECK(st.packs() == 3);      // all three ring entries are retained packs
    }
    CHECK(live_frames() == base_frames + 3);
    CHECK(pool_live()   == base_pool + 3);

    // ---------------------------------------------------------------------
    // (C) Replay re-emits the SAME sealed handle, fresh id, zero pixel copy.
    //     (run before eviction so we replay a known ring.)
    // ---------------------------------------------------------------------
    SECTION("replay_all: same handles re-emitted, fresh ids, pool live-count stable");
    {
        drain_events();
        const int pool_before = pool_live();
        const size_t frames_before = live_frames();
        adapter->exchange(xi::cache::Command::replay_all());   // synchronous emit

        std::lock_guard<std::mutex> lk(g_mu);
        CHECK(g_events.size() == 3);
        // Zero copy: replay minted NO new pack and NO new pixel buffer. The pool
        // live-count is unchanged (the ring still owns each pack; the transient
        // event refs have not been drained yet, but they alias the SAME handles).
        CHECK(live_frames() == frames_before);
        CHECK(pool_live()   == pool_before);
        for (size_t i = 0; i < g_events.size(); ++i) {
            CHECK(g_events[i].pack != XI_PACK_NULL);
            // Fresh trigger id on every re-emission.
            CHECK(!xi_trigger_id_is_null(g_events[i].id));
            // Identical sealed content: the seq marker + image bytes survive.
            int64_t seq = -1;
            CHECK(g_fi->get_i64(g_events[i].pack, "seq", &seq) == 1 && seq == (int64_t)i);
            xi_pack_image img{};
            CHECK(g_fi->get_image(g_events[i].pack, "img", &img) == 1);
            CHECK(img.width == 64 && img.height == 64 && img.channels == 1);
            const uint8_t* px = static_cast<const uint8_t*>(img.pixels);
            const uint8_t expect = (uint8_t)(10 * (i + 1));   // 10,20,30
            CHECK(px && px[0] == expect && px[(64 * 64) - 1] == expect);
        }
    }
    drain_events();
    // After draining the event refs, the ring's own refs are all that remain.
    CHECK(live_frames() == base_frames + 3);
    CHECK(pool_live()   == base_pool + 3);

    // ---------------------------------------------------------------------
    // (B) Eviction releases: shrink capacity to 2 → the oldest pack is dropped.
    // ---------------------------------------------------------------------
    SECTION("set_capacity(2): eviction releases the evicted pack (both oracles fall)");
    adapter->exchange(xi::cache::Command::set_capacity(2));
    {
        auto st = status(*adapter);
        CHECK(st.capacity() == 2);
        CHECK(st.count() == 2);
        CHECK(st.packs() == 2);
    }
    CHECK(live_frames() == base_frames + 2);   // one sealed pack fully released
    CHECK(pool_live()   == base_pool + 2);     // its pool image freed with it

    // clear releases the rest.
    SECTION("clear: releases every retained pack back to baseline");
    adapter->exchange(xi::cache::Command::clear());
    CHECK(status(*adapter).count() == 0);
    CHECK(live_frames() == base_frames);
    CHECK(pool_live()   == base_pool);

    // ---------------------------------------------------------------------
    // (D) Mixed ring: record + pack interleaved, replayed in capture order.
    // ---------------------------------------------------------------------
    SECTION("mixed ring: pack, record, pack replay in capture order");
    adapter->exchange(xi::cache::Command::set_capacity(8));
    capture_pack(*adapter, make_pack(100, 40), 1);   // slot 0: pack
    capture_record(*adapter);                        // slot 1: record
    capture_pack(*adapter, make_pack(101, 50), 3);   // slot 2: pack
    {
        auto st = status(*adapter);
        CHECK(st.count() == 3);
        CHECK(st.packs() == 2);        // two packs, one record
    }
    {
        drain_events();
        adapter->exchange(xi::cache::Command::replay_all());
        std::lock_guard<std::mutex> lk(g_mu);
        CHECK(g_events.size() == 3);
        if (g_events.size() == 3) {
            // Order preserved across the variant entries.
            int64_t s0 = -1, s2 = -1;
            CHECK(g_events[0].pack != XI_PACK_NULL);                 // pack (seq 100)
            CHECK(g_fi->get_i64(g_events[0].pack, "seq", &s0) == 1 && s0 == 100);
            CHECK(g_events[1].pack == XI_PACK_NULL);                 // the record, mid-ring
            CHECK(g_events[2].pack != XI_PACK_NULL);                 // pack (seq 101)
            CHECK(g_fi->get_i64(g_events[2].pack, "seq", &s2) == 1 && s2 == 101);
        }
    }
    drain_events();

    // ---------------------------------------------------------------------
    // (E) Teardown releases everything (the ring still holds 2 packs + 1 record).
    // ---------------------------------------------------------------------
    SECTION("destroy: ring teardown releases all retained packs");
    adapter.reset();     // ~CAbiInstanceAdapter -> xi_plugin_destroy -> ~BufferReplay
    FreeLibrary(dll);
    xi::TriggerBus::instance().clear_sink();

    CHECK(live_frames() == base_frames);
    CHECK(pool_live()   == base_pool);

    if (g_failures == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
