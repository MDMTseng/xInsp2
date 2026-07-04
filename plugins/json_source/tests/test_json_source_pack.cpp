//
// test_json_source_pack.cpp — the json_source pack-door proof (docs/new_gen/10
// Gate P1) against the REAL built DLL, mirroring pack_pilot_test.
//
// v12: the Record process path is deleted, so json_source is a PACK-DOOR SOURCE.
// The host TICKS its xi.pack@1 door once per frame; json_source ignores the tick
// input and EMITS the stored (GUI-edited) document as a sealed pack into
// dispatch. This test loads the genuine DLL through the C ABI + host adapter,
// installs the real pack ABI + trigger bus, ticks the door, and captures the
// emitted pack off the bus:
//
//   * EMIT — one tick emits ONE sealed pack: the document's top-level scalars
//     land as canonical pack entries (bool → bool tag, number → i64/f64,
//     string → str), a nested object/array lands as ONE ingress-canonicalized
//     `mp` entry that decodes back, and a leading `seq` counter mirrors
//     mock_camera (free-running, one per emitted pack).
//   * HOSTILE — a depth-bomb document is rejected LOUDLY: a sealed $fault pack
//     (never a partial/silent result), per the doc-07 ingress semantics.
//   * A non-object document root is rejected loudly too.
//   * Pooled-handle balance across the whole run.
//
// (v12: the pack_mode-OFF Record-parity leg and the per-tick input-patch leg are
// gone — the Record plane was deleted and the pack tick carries no patch input;
// patches are applied to the stored def via exchange/set_def.)
//
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include <xi/xi_cabi_adapter.hpp>   // CAbiInstanceAdapter (has_pack_door / run_pack_door)
#include <xi/xi_contract.hpp>       // shared fault reason codes (kWrongType)
#include <xi/xi_image_pool.hpp>     // make_host_api + cumulative().live_now
#include <xi/xi_pack_abi.hpp>       // install_pack_abi + pack_v1_iface + PackRegistry
#include <xi/xi_trigger_bus.hpp>    // the dispatch sink (TriggerBus)
#include <xi/xi_mp.hpp>             // decode the nested-object mp entry

#include "json_source_keys.gen.h"

#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifndef JSON_SOURCE_DLL_PATH
#define JSON_SOURCE_DLL_PATH "xi-json_source.dll"
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

namespace jkeys = xi::json_source::keys;

static int pool_live() { return xi::ImagePool::instance().cumulative().live_now; }

// One captured drive: the packs a single door tick emitted to the bus.
struct Captured {
    std::vector<xi::TriggerEvent> events;
    xi_pack_handle first_pack() const {
        for (auto& ev : events) if (ev.pack != XI_PACK_NULL) return ev.pack;
        return XI_PACK_NULL;
    }
    int pack_count() const {
        int n = 0; for (auto& ev : events) if (ev.pack != XI_PACK_NULL) ++n; return n;
    }
    void release_all() {
        for (auto& ev : events)
            if (ev.pack != XI_PACK_NULL) xi::TriggerBus::instance().release_pack_(ev.pack);
        events.clear();
    }
};

// Tick the source's pack door once (empty input pack — the door ignores it) and
// return the packs it emitted to the bus during the tick. The sealed (empty) ack
// pack the door returns is released here.
static Captured tick_capturing(const xi_pack_v1* fi, xi::CAbiInstanceAdapter& a) {
    Captured cap;
    xi::TriggerBus::instance().set_sink([&](xi::TriggerEvent ev) {
        cap.events.push_back(std::move(ev));
    });
    xi_pack_handle in  = fi->builder_seal(fi->builder_new());
    xi_pack_handle ack = a.run_pack_door(in);
    if (ack != XI_PACK_NULL) fi->release(ack);
    fi->release(in);
    xi::TriggerBus::instance().clear_sink();
    return cap;
}

int main() {
    std::printf("[test] json_source pack-door source (real DLL through xi.pack@1)\n");

    xi::install_pack_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    // [v12 THE CUT: the Record trigger hook is gone — install_pack_abi() wires the emit_pack forwarder]
    const xi_pack_v1* fi = xi::pack_v1_iface();

    const int base = pool_live();

    HMODULE dll = LoadLibraryA(JSON_SOURCE_DLL_PATH);
    if (!dll) { std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", JSON_SOURCE_DLL_PATH, GetLastError()); return 1; }
    auto factory   = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(dll, "xi_plugin_create"));
    auto get_iface = reinterpret_cast<xi_plugin_get_interface_fn>(GetProcAddress(dll, "xi_plugin_get_interface"));
    CHECK(factory != nullptr);
    if (!factory) return 1;
    void* inst = factory(&host, "src");
    CHECK(inst != nullptr);
    if (!inst) { std::fprintf(stderr, "\nSETUP FAILED\n"); return 1; }
    auto p = std::make_unique<xi::CAbiInstanceAdapter>("src", "src", dll, inst);

    // ---------------------------------------------------------------------
    // Door probe: json_source publishes the xi.pack@1 door.
    // ---------------------------------------------------------------------
    SECTION("door probe: json_source answers xi.pack@1");
    CHECK(get_iface && get_iface("xi.pack", 1) != nullptr);
    CHECK(p->has_pack_door());

    // ---------------------------------------------------------------------
    // (A) EMIT: scalars → canonical entries, nested → one canonical mp entry,
    //     plus a leading seq. Seed the document through the config wrapper.
    // ---------------------------------------------------------------------
    SECTION("tick emits a sealed pack: scalars + canonicalized nested payload + seq");
    {
        CHECK(p->set_def(R"({"data":{)"
                         R"("n":42,"ratio":1.5,"name":"widget","ok":true,)"
                         R"("roi":{"x":1,"y":2,"pts":[3,4,5]})"
                         R"(}})"));
        Captured cap = tick_capturing(fi, *p);
        CHECK(cap.pack_count() == 1);
        xi_pack_handle pk = cap.first_pack();
        CHECK(pk != XI_PACK_NULL);
        if (pk) {
            // seq leads (mirrors mock_camera) — first emit is 0.
            int64_t seq = -1;
            CHECK(fi->get_i64(pk, jkeys::kSeq, &seq) == 1 && seq == 0);
            // scalar entries, canonical-profile typed.
            int64_t n = 0;    CHECK(fi->get_i64(pk, "n", &n) == 1 && n == 42);
            double ratio = 0; CHECK(fi->get_f64(pk, "ratio", &ratio) == 1 && ratio == 1.5);
            const char* s = nullptr; int32_t sl = 0;
            CHECK(fi->get_str(pk, "name", &s, &sl) == 1 && std::string(s, (size_t)sl) == "widget");
            // bool → a REAL bool entry (tag XI_PACK_TAG_BOOL) — the i64 read
            // fail-closes on the bool tag.
            CHECK(fi->tag_of(pk, "ok") == XI_PACK_TAG_BOOL);
            int32_t okb = 0; CHECK(fi->get_bool(pk, "ok", &okb) == 1 && okb == 1);
            int64_t ok = -1; CHECK(fi->get_i64(pk, "ok", &ok) == 0);
            // not a fault pack.
            const char* fc = nullptr; int32_t fl = 0;
            CHECK(fi->get_str(pk, "$fault", &fc, &fl) == 0);
            // nested "roi" rode as ONE mp entry — decode it back through the codec.
            const void* mp = nullptr; int32_t ml = 0;
            CHECK(fi->get_mp(pk, "roi", &mp, &ml) == 1 && ml > 0);
            if (mp && ml > 0) {
                CHECK(xi::mp::validate(static_cast<const uint8_t*>(mp), (size_t)ml)
                          == xi::mp::Status::Ok);
                xi::mp::Reader r(static_cast<const uint8_t*>(mp), (size_t)ml);
                xi::mp::Element top;
                CHECK(r.next(top) == xi::mp::Status::Ok && top.kind == xi::mp::Kind::Map);
                int64_t roi_x = -1, pts_len = -1;
                for (uint32_t i = 0; i < top.len; ++i) {
                    xi::mp::Element key, val;
                    CHECK(r.next(key) == xi::mp::Status::Ok && key.kind == xi::mp::Kind::Str);
                    std::string k(reinterpret_cast<const char*>(key.data), key.len);
                    CHECK(r.next(val) == xi::mp::Status::Ok);
                    if (k == "x" && val.kind == xi::mp::Kind::Int) roi_x = val.i;
                    else if (k == "pts" && val.kind == xi::mp::Kind::Array) {
                        pts_len = (int64_t)val.len;
                        for (uint32_t j = 0; j < val.len; ++j) { xi::mp::Element e; r.next(e); }
                    } else if (val.kind == xi::mp::Kind::Map) {
                        for (uint32_t j = 0; j < val.len * 2; ++j) { xi::mp::Element e; r.next(e); }
                    }
                }
                CHECK(roi_x == 1);
                CHECK(pts_len == 3);
            }
        }
        cap.release_all();

        // seq advances per emit (mirrors mock_camera's free-running counter).
        Captured cap2 = tick_capturing(fi, *p);
        xi_pack_handle pk2 = cap2.first_pack();
        int64_t seq2 = -1;
        CHECK(pk2 && fi->get_i64(pk2, jkeys::kSeq, &seq2) == 1 && seq2 == 1);
        cap2.release_all();
    }

    // ---------------------------------------------------------------------
    // (B) HOSTILE: a depth-bomb document is rejected loudly with a $fault pack.
    // ---------------------------------------------------------------------
    SECTION("depth-bomb document -> sealed $fault pack (fail loud)");
    {
        std::string deep;
        const int D = xi::mp::kDefaultMaxDepth + 8;
        for (int i = 0; i < D; ++i) deep += "{\"a\":";
        deep += "1";
        for (int i = 0; i < D; ++i) deep += "}";
        std::string cfg = std::string(R"({"data":{"deep":)") + deep + "}}";
        CHECK(p->set_def(cfg));

        Captured cap = tick_capturing(fi, *p);
        CHECK(cap.pack_count() == 1);                 // a fault is still a sealed pack
        xi_pack_handle pk = cap.first_pack();
        if (pk) {
            const char* code = nullptr; int32_t cl = 0;
            CHECK(fi->get_str(pk, "$fault", &code, &cl) == 1);
            CHECK(cl > 0 && std::string(code, (size_t)cl) == "depth-exceeded");
            const char* key = nullptr; int32_t kl = 0;
            CHECK(fi->get_str(pk, "$fault_key", &key, &kl) == 1 &&
                  std::string(key, (size_t)kl) == "deep");
            // seq is still stamped; no user scalars leaked onto the fault pack.
            int64_t seq = -1; CHECK(fi->get_i64(pk, jkeys::kSeq, &seq) == 1);
        }
        cap.release_all();
    }

    // ---------------------------------------------------------------------
    // (C) A non-object document root is rejected loudly too.
    // ---------------------------------------------------------------------
    SECTION("non-object root -> $fault (wrong_type)");
    {
        CHECK(p->set_def(R"({"data":[1,2,3]})"));
        Captured cap = tick_capturing(fi, *p);
        xi_pack_handle pk = cap.first_pack();
        if (pk) {
            const char* code = nullptr; int32_t cl = 0;
            CHECK(fi->get_str(pk, "$fault", &code, &cl) == 1 &&
                  std::string(code, (size_t)cl) == xi::contract::kWrongType);
        }
        cap.release_all();
    }

    // ---------------------------------------------------------------------
    // Teardown + pooled-handle balance.
    // ---------------------------------------------------------------------
    p.reset();               // ~CAbiInstanceAdapter -> xi_plugin_destroy
    FreeLibrary(dll);
    xi::TriggerBus::instance().clear_sink();

    CHECK(pool_live() == base);
    CHECK(xi::PackRegistry::instance().live_frames() == 0);

    if (g_failures == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
