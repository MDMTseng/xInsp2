//
// test_json_source_pack.cpp — the polaris2 wave-2 BILINGUAL proof for json_source
// against the REAL built DLL (docs/new_gen/10 Gate P1, mirroring pack_pilot_test).
//
// json_source is a SOURCE: in pack_mode it emits each (patched) document as a
// sealed xi.pack@1 pack through the host door instead of returning a Record — the
// same emit path mock_camera takes. This test loads the genuine DLL through the C
// ABI, installs the real pack ABI + trigger bus, and drives process() while a bus
// sink captures the emitted pack:
//
//   * OFF (default) — pack_mode false: process() returns the Record byte-for-byte
//     as before and NOTHING reaches the pack bus (the Record path is unchanged).
//   * ON — the document's top-level scalars land as canonical pack entries
//     (bool → bool tag, number → i64/f64, string → str), a nested object/array
//     lands as ONE ingress-canonicalized `mp` entry that decodes back, and a
//     leading `seq` counter mirrors mock_camera.
//   * HOSTILE — a depth-bomb document is rejected LOUDLY: a sealed $fault pack
//     (never a partial/silent result), per the doc-07 ingress semantics.
//   * Pooled-handle balance across the whole run.
//
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include <xi/xi_abi.hpp>            // xi_record / xi_record_out C ABI
#include <xi/xi_contract.hpp>      // shared fault reason codes (kWrongType)
#include <xi/xi_image_pool.hpp>     // make_host_api + cumulative().live_now
#include <xi/xi_pack_abi.hpp>       // install_pack_abi + pack_v1_iface + PackRegistry
#include <xi/xi_trigger_bus.hpp>    // install_trigger_hook + the dispatch sink
#include <xi/xi_mp.hpp>             // decode the nested-object mp entry

#include "json_source_keys.gen.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
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

// The loaded C-ABI plugin.
struct Plugin {
    HMODULE dll = nullptr;
    void*   inst = nullptr;
    xi_plugin_create_fn   create   = nullptr;
    xi_plugin_destroy_fn  destroy  = nullptr;
    xi_plugin_process_fn  process  = nullptr;
    xi_plugin_set_def_fn  set_def  = nullptr;
    xi_plugin_get_def_fn  get_def  = nullptr;
};

static Plugin load_plugin(const char* path, const xi_host_api* host) {
    Plugin p;
    p.dll = LoadLibraryA(path);
    if (!p.dll) { std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", path, GetLastError()); ++g_failures; return p; }
    p.create  = (xi_plugin_create_fn)  GetProcAddress(p.dll, "xi_plugin_create");
    p.destroy = (xi_plugin_destroy_fn) GetProcAddress(p.dll, "xi_plugin_destroy");
    p.process = (xi_plugin_process_fn) GetProcAddress(p.dll, "xi_plugin_process");
    p.set_def = (xi_plugin_set_def_fn) GetProcAddress(p.dll, "xi_plugin_set_def");
    p.get_def = (xi_plugin_get_def_fn) GetProcAddress(p.dll, "xi_plugin_get_def");
    CHECK(p.create && p.destroy && p.process && p.set_def && p.get_def);
    if (p.create) p.inst = p.create(host, "src");
    CHECK(p.inst != nullptr);
    return p;
}

// Drive one process() call with a JSON input record (patches), returning the
// output Record's serialized JSON (empty images assumed — json_source is metadata).
static std::string run_process(Plugin& p, const std::string& in_json) {
    xi_record in{};
    in.images      = nullptr;
    in.image_count = 0;
    in.data        = reinterpret_cast<const uint8_t*>(in_json.c_str());
    in.len         = static_cast<int32_t>(in_json.size());

    xi_record_out out; xi_record_out_init(&out);
    p.process(p.inst, &in, &out);

    std::string js;
    if (out.out_doc) {
        size_t n = 0;
        char* s = yyjson_mut_write(reinterpret_cast<yyjson_mut_doc*>(out.out_doc), 0, &n);
        js = s ? std::string(s, n) : std::string();
        if (s) free(s);
    } else if (out.data) {
        js = std::string(reinterpret_cast<const char*>(out.data), (size_t)out.len);
    }
    xi_record_out_free(&out);
    return js;
}

// Capture-and-drain: run process() once and return the FIRST pack emitted to the
// bus during the call (XI_PACK_NULL if none). The returned handle is still owned
// by the captured event; the caller releases it via TriggerBus::release_pack_.
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

static Captured process_capturing(Plugin& p, const std::string& in_json) {
    Captured cap;
    xi::TriggerBus::instance().set_sink([&](xi::TriggerEvent ev) {
        cap.events.push_back(std::move(ev));
    });
    run_process(p, in_json);
    xi::TriggerBus::instance().clear_sink();
    return cap;
}

int main() {
    std::printf("[test] json_source bilingual pack-mode (real DLL through xi.pack@1)\n");

    xi::install_pack_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_trigger_hook(host);
    const xi_pack_v1* fi = xi::pack_v1_iface();

    const int base = pool_live();

    Plugin p = load_plugin(JSON_SOURCE_DLL_PATH, &host);
    if (!p.inst) { std::fprintf(stderr, "\nSETUP FAILED\n"); return 1; }

    // ---------------------------------------------------------------------
    // (A) OFF (default): the Record path is unchanged and nothing reaches the
    //     pack bus. Seed a document via the config wrapper (pack_mode absent).
    // ---------------------------------------------------------------------
    SECTION("pack_mode OFF: Record emit unchanged, zero packs on the bus");
    {
        CHECK(p.set_def(p.inst, R"({"data":{"n":5,"tag":"hi"}})") == 0);
        Captured cap = process_capturing(p, "{}");
        CHECK(cap.pack_count() == 0);                 // no pack emitted in Record mode
        // The returned Record is the stored document (a raw-key read; open output).
        auto rec_js = run_process(p, "{}");
        CHECK(rec_js.find("\"n\":5") != std::string::npos);
        CHECK(rec_js.find("\"tag\":\"hi\"") != std::string::npos);
        cap.release_all();
    }

    // ---------------------------------------------------------------------
    // (B) ON: scalars → canonical entries, nested → one canonical mp entry,
    //     plus a leading seq. Turn pack_mode on through the config wrapper.
    // ---------------------------------------------------------------------
    SECTION("pack_mode ON: scalars + canonicalized nested payload + seq");
    {
        CHECK(p.set_def(p.inst, R"({"pack_mode":true,"data":{)"
                                R"("n":42,"ratio":1.5,"name":"widget","ok":true,)"
                                R"("roi":{"x":1,"y":2,"pts":[3,4,5]})"
                                R"(}})") == 0);
        Captured cap = process_capturing(p, "{}");
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
            // bool → a REAL bool entry (tag XI_PACK_TAG_BOOL) — the old i64 0/1
            // workaround is gone: the i64 read now fail-closes on the bool tag.
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
                // Structurally valid canonical msgpack (the ingress edge proved it).
                CHECK(xi::mp::validate(static_cast<const uint8_t*>(mp), (size_t)ml)
                          == xi::mp::Status::Ok);
                // Walk the top-level map, tap roi.x / roi.pts to prove content survived.
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
        Captured cap2 = process_capturing(p, "{}");
        xi_pack_handle pk2 = cap2.first_pack();
        int64_t seq2 = -1;
        CHECK(pk2 && fi->get_i64(pk2, jkeys::kSeq, &seq2) == 1 && seq2 == 1);
        cap2.release_all();
    }

    // ---------------------------------------------------------------------
    // (C) A per-emit patch mutates ONLY the emitted pack (stored doc untouched),
    //     exactly as it does on the Record path.
    // ---------------------------------------------------------------------
    SECTION("pack_mode ON: input patch mutates the emitted pack");
    {
        Captured cap = process_capturing(p, R"({"key":".n","value":99})");
        xi_pack_handle pk = cap.first_pack();
        int64_t n = -1;
        CHECK(pk && fi->get_i64(pk, "n", &n) == 1 && n == 99);   // patched value
        cap.release_all();
    }

    // ---------------------------------------------------------------------
    // (D) HOSTILE: a depth-bomb document is rejected loudly with a $fault pack.
    // ---------------------------------------------------------------------
    SECTION("pack_mode ON: depth-bomb document -> sealed $fault pack (fail loud)");
    {
        // Build {"deep":{{{...}}}} nested well beyond the ingress depth limit.
        std::string deep;
        const int D = xi::mp::kDefaultMaxDepth + 8;
        for (int i = 0; i < D; ++i) deep += "{\"a\":";
        deep += "1";
        for (int i = 0; i < D; ++i) deep += "}";
        std::string cfg = std::string(R"({"pack_mode":true,"data":{"deep":)") + deep + "}}";
        CHECK(p.set_def(p.inst, cfg.c_str()) == 0);

        Captured cap = process_capturing(p, "{}");
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
    // (E) A non-object document root is rejected loudly too.
    // ---------------------------------------------------------------------
    SECTION("pack_mode ON: non-object root -> $fault (wrong_type)");
    {
        CHECK(p.set_def(p.inst, R"({"pack_mode":true,"data":[1,2,3]})") == 0);
        Captured cap = process_capturing(p, "{}");
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
    p.destroy(p.inst);
    FreeLibrary(p.dll);

    CHECK(pool_live() == base);
    CHECK(xi::PackRegistry::instance().live_frames() == 0);

    if (g_failures == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
