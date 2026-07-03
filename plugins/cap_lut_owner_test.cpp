//
// cap_lut_owner_test.cpp — demo.lut (the resource-handle pattern demo type
// owner, doc 14 appendix) end to end against the REAL built DLL through the
// REAL capability plane: heavy objects stay inside the owner DLL, packs carry
// only the handle entry {type,id,gen,$v}, and THE ZERO-REBUILD HEADLINE — the
// same sealed content handed to two consumers builds ONCE (builds == 1,
// built=0 dedup answer, identical handle bytes) while both query it.
//
// Also: dump byte-determinism (the persistence materializer), stale leases
// via ring pressure AND via recycle_all (both -> $fault stale_handle),
// wrong-type resolve -> $fault wrong_type, malformed / out-of-range handles
// -> bad_handle, the handle-$v gate, $probe/$v on all three capabilities,
// stats via exchange, unregister-on-destroy, and pack-registry balance.
//
// Host role exactly like cap_imgcodec_test: ImagePool host_api +
// install_pack_abi + install_cap_plane; the factory runs under a pre-allocated
// owner scope and the adapter adopts it (the PM create path).
//
#include <xi/xi_abi.h>
#include <xi/xi_cap_abi.hpp>
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_instance.hpp>
#include <xi/xi_mp.hpp>
#include <xi/xi_pack_abi.hpp>
#include <xi/xi_seh.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef LUT_OWNER_DLL_PATH
#define LUT_OWNER_DLL_PATH "lut_owner/xi-lut_owner.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::fprintf(stderr, "\n[section] %s\n", name)

static int jint(const std::string& s, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    auto p = s.find(pat);
    if (p == std::string::npos) return -12345;
    p = s.find(':', p);
    if (p == std::string::npos) return -12345;
    return std::atoi(s.c_str() + p + 1);
}
static std::string pstr(const xi_pack_v1* pk, xi_pack_handle h, const char* key) {
    const char* p = nullptr; int32_t n = 0;
    if (h != XI_PACK_NULL && pk->get_str(h, key, &p, &n) && p)
        return std::string(p, (size_t)n);
    return "";
}
static int64_t pi64(const xi_pack_v1* pk, xi_pack_handle h, const char* key, int64_t d) {
    int64_t v = d;
    if (h != XI_PACK_NULL) pk->get_i64(h, key, &v);
    return v;
}
static std::vector<uint8_t> pbin(const xi_pack_v1* pk, xi_pack_handle h, const char* key) {
    const void* p = nullptr; int32_t n = 0;
    if (h != XI_PACK_NULL && pk->get_bin(h, key, &p, &n) && p && n > 0)
        return std::vector<uint8_t>(static_cast<const uint8_t*>(p),
                                    static_cast<const uint8_t*>(p) + n);
    return {};
}
static std::vector<uint8_t> pmp(const xi_pack_v1* pk, xi_pack_handle h, const char* key) {
    const void* p = nullptr; int32_t n = 0;
    if (h != XI_PACK_NULL && pk->get_mp(h, key, &p, &n) && p && n > 0)
        return std::vector<uint8_t>(static_cast<const uint8_t*>(p),
                                    static_cast<const uint8_t*>(p) + n);
    return {};
}

// Decode a handle entry {type,id,gen,$v} from its canonical-mp bytes.
struct Handle { std::string type; int64_t id = -1, gen = -1, v = -1; bool ok = false; };
static Handle parse_handle(const std::vector<uint8_t>& mp) {
    Handle h;
    xi::mp::Reader r(mp.data(), mp.size());
    xi::mp::Element e;
    if (r.next(e) != xi::mp::Status::Ok || e.kind != xi::mp::Kind::Map) return h;
    for (uint32_t i = 0; i < e.len; ++i) {
        xi::mp::Element k, v;
        if (r.next(k) != xi::mp::Status::Ok || k.kind != xi::mp::Kind::Str) return h;
        if (r.next(v) != xi::mp::Status::Ok) return h;
        std::string key(reinterpret_cast<const char*>(k.data), k.len);
        if (key == "type" && v.kind == xi::mp::Kind::Str)
            h.type.assign(reinterpret_cast<const char*>(v.data), v.len);
        else if (key == "id")  h.id  = v.i;
        else if (key == "gen") h.gen = v.i;
        else if (key == "$v")  h.v   = v.i;
    }
    h.ok = true;
    return h;
}

// Build a canonical i64 array entry value.
static xi::mp::Writer i64_array(const std::vector<int64_t>& xs) {
    xi::mp::Writer w;
    w.array((uint32_t)xs.size());
    for (int64_t x : xs) w.int_(x);
    return w;
}

// One build request: keys+values arrays (+$v 1).
static xi_pack_handle build_req(const xi_pack_v1* pk,
                                const std::vector<int64_t>& keys,
                                const std::vector<int64_t>& vals) {
    xi_pack_builder b = pk->builder_new();
    auto kw = i64_array(keys), vw = i64_array(vals);
    pk->builder_add_mp(b, "keys",   kw.bytes().data(), (int32_t)kw.bytes().size());
    pk->builder_add_mp(b, "values", vw.bytes().data(), (int32_t)vw.bytes().size());
    pk->builder_add_i64(b, "$v", 1);
    return pk->builder_seal(b);
}

// One query/dump request from raw handle bytes.
static xi_pack_handle handle_req(const xi_pack_v1* pk,
                                 const std::vector<uint8_t>& handle_mp,
                                 const std::vector<int64_t>* qkeys) {
    xi_pack_builder b = pk->builder_new();
    pk->builder_add_mp(b, "handle", handle_mp.data(), (int32_t)handle_mp.size());
    if (qkeys) {
        auto kw = i64_array(*qkeys);
        pk->builder_add_mp(b, "keys", kw.bytes().data(), (int32_t)kw.bytes().size());
    }
    return pk->builder_seal(b);
}

// Read the i64-or-nil "values" answer array.
static bool read_values(const xi_pack_v1* pk, xi_pack_handle h,
                        std::vector<int64_t>* out, int* nils) {
    auto mp = pmp(pk, h, "values");
    if (mp.empty()) return false;
    xi::mp::Reader r(mp.data(), mp.size());
    xi::mp::Element e;
    if (r.next(e) != xi::mp::Status::Ok || e.kind != xi::mp::Kind::Array) return false;
    out->clear(); *nils = 0;
    for (uint32_t i = 0; i < e.len; ++i) {
        xi::mp::Element el;
        if (r.next(el) != xi::mp::Status::Ok) return false;
        if (el.kind == xi::mp::Kind::Int) out->push_back(el.i);
        else if (el.kind == xi::mp::Kind::Nil) ++*nils;
        else return false;
    }
    return true;
}

int main() {
    std::printf("[test] demo.lut — resource-handle convention through the capability plane\n");
    xi::install_seh_translator();

    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();
    xi::install_cap_plane();
    const auto* pk  = static_cast<const xi_pack_v1*>(host.get_interface("xi.pack", 1));
    const auto* cap = static_cast<const xi_cap_v1*>(host.get_interface("xi.cap", 1));
    CHECK(pk && cap);
    if (!pk || !cap) return 1;
    const size_t frames_baseline = xi::PackRegistry::instance().live_frames();

    SECTION("load the REAL xi-lut_owner.dll; register-on-create through the PM path");
    HMODULE dll = LoadLibraryA(LUT_OWNER_DLL_PATH);
    if (!dll) {
        std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n",
                     LUT_OWNER_DLL_PATH, GetLastError());
        return 1;
    }
    auto create = reinterpret_cast<xi::PluginInfo::CFactoryFn>(
        GetProcAddress(dll, "xi_plugin_create"));
    CHECK(create != nullptr);
    if (!create) return 1;

    std::shared_ptr<xi::CAbiInstanceAdapter> lut;
    {
        xi::ImagePoolOwnerScope scope;
        void* raw = scope.run_factory([&] { return create(&host, "lut"); });
        CHECK(raw != nullptr);
        if (!raw) return 1;
        lut = std::make_shared<xi::CAbiInstanceAdapter>(
            "lut", "lut_owner", dll, raw, /*reentrant=*/true, /*max_conc=*/0);
        lut->adopt_owner_id(scope.release());
        xi::InstanceRegistry::instance().add(lut);
    }
    CHECK(cap->available("demo.lut.build") == 1);
    CHECK(cap->available("demo.lut.query") == 1);
    CHECK(cap->available("demo.lut.dump")  == 1);
    CHECK(lut->set_def("{\"ring_slots\":3}"));   // small ring: pressure is testable

    const std::vector<int64_t> K{10, 20, 30, 40}, V{11, 22, 33, 44};

    SECTION("build -> handle entry {type,id,gen,$v}; the object stays inside the DLL");
    std::vector<uint8_t> hA;                        // consumer 1's handle bytes
    {
        xi_pack_handle req = build_req(pk, K, V);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("demo.lut.build", req, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault").empty());
        CHECK(pi64(pk, out, "built", -1) == 1);
        CHECK(pi64(pk, out, "builds", -1) == 1);
        CHECK(pi64(pk, out, "size", -1) == 4);
        hA = pmp(pk, out, "handle");
        CHECK(!hA.empty());
        Handle h = parse_handle(hA);
        CHECK(h.ok && h.type == "demo.lut" && h.id >= 0 && h.gen >= 1 && h.v == 1);
        pk->release(out); pk->release(req);
    }

    SECTION("consumer 2, same sealed content: ZERO REBUILD (built=0, builds "
            "pinned at 1, byte-identical handle) — the dedup proof");
    {
        xi_pack_handle req = build_req(pk, K, V);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("demo.lut.build", req, &out) == XI_CAP_OK);
        CHECK(pi64(pk, out, "built", -1) == 0);
        CHECK(pi64(pk, out, "builds", -1) == 1);              // counter did NOT move
        CHECK(pmp(pk, out, "handle") == hA);                  // the SAME lease
        pk->release(out); pk->release(req);
        std::string s = lut->exchange("{\"command\":\"stats\"}");
        CHECK(jint(s, "builds") == 1);
        CHECK(jint(s, "dedup_hits") == 1);
        CHECK(jint(s, "live") == 1);
    }

    SECTION("both consumers query through the handle (found values, builds echo)");
    {
        const std::vector<int64_t> q1{20, 40}, q2{10, 30, 999};
        xi_pack_handle req = handle_req(pk, hA, &q1);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("demo.lut.query", req, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault").empty());
        std::vector<int64_t> vals; int nils = 0;
        CHECK(read_values(pk, out, &vals, &nils));
        CHECK(nils == 0 && vals == std::vector<int64_t>({22, 44}));
        CHECK(pi64(pk, out, "found", -1) == 2);
        CHECK(pi64(pk, out, "builds", -1) == 1);              // still ONE build
        pk->release(out); pk->release(req);

        req = handle_req(pk, hA, &q2);                        // missing key -> nil
        CHECK(cap->call("demo.lut.query", req, &out) == XI_CAP_OK);
        CHECK(read_values(pk, out, &vals, &nils));
        CHECK(nils == 1 && vals == std::vector<int64_t>({11, 33}));
        CHECK(pi64(pk, out, "found", -1) == 2);
        pk->release(out); pk->release(req);
    }

    SECTION("dump: the materializer is BYTE-DETERMINISTIC (rule 4)");
    std::vector<uint8_t> dump1;
    {
        xi_pack_handle req = handle_req(pk, hA, nullptr);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("demo.lut.dump", req, &out) == XI_CAP_OK);
        dump1 = pbin(pk, out, "lut");
        CHECK(dump1.size() == 9 + 4 * 16);
        CHECK(dump1.size() >= 4 && dump1[0] == 'X' && dump1[1] == 'L' &&
              dump1[2] == 'U' && dump1[3] == 'T');
        pk->release(out); pk->release(req);

        req = handle_req(pk, hA, nullptr);                    // dump again
        CHECK(cap->call("demo.lut.dump", req, &out) == XI_CAP_OK);
        CHECK(pbin(pk, out, "lut") == dump1);                 // byte-identical
        pk->release(out); pk->release(req);
    }

    SECTION("wrong-type resolve -> $fault wrong_type (rule 5)");
    {
        xi::mp::Writer w;
        w.map(4);
        w.key("type"); w.str("demo.image");
        w.key("id");   w.int_(0);
        w.key("gen");  w.int_(1);
        w.key("$v");   w.int_(1);
        std::vector<uint8_t> bad(w.bytes().begin(), w.bytes().end());
        const std::vector<int64_t> q{10};
        xi_pack_handle req = handle_req(pk, bad, &q);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("demo.lut.query", req, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "wrong_type");
        pk->release(out); pk->release(req);
    }

    SECTION("malformed / out-of-range handles -> bad_handle; handle $v gate");
    {
        // Not a map.
        xi::mp::Writer w1; w1.int_(7);
        std::vector<uint8_t> notmap(w1.bytes().begin(), w1.bytes().end());
        xi_pack_handle req = handle_req(pk, notmap, nullptr);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("demo.lut.dump", req, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "bad_handle");
        pk->release(out); pk->release(req);

        // Right type, id far out of the ring.
        xi::mp::Writer w2;
        w2.map(4);
        w2.key("type"); w2.str("demo.lut");
        w2.key("id");   w2.int_(999);
        w2.key("gen");  w2.int_(1);
        w2.key("$v");   w2.int_(1);
        std::vector<uint8_t> oob(w2.bytes().begin(), w2.bytes().end());
        req = handle_req(pk, oob, nullptr);
        CHECK(cap->call("demo.lut.dump", req, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "bad_handle");
        pk->release(out); pk->release(req);

        // Handle schema version not spoken.
        xi::mp::Writer w3;
        w3.map(4);
        w3.key("type"); w3.str("demo.lut");
        w3.key("id");   w3.int_(0);
        w3.key("gen");  w3.int_(1);
        w3.key("$v");   w3.int_(9);
        std::vector<uint8_t> badv(w3.bytes().begin(), w3.bytes().end());
        req = handle_req(pk, badv, nullptr);
        CHECK(cap->call("demo.lut.dump", req, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "unsupported_version");
        pk->release(out); pk->release(req);
    }

    SECTION("ring pressure: LRU recycle bumps the generation -> stale_handle (rule 3)");
    {
        // ring_slots=3, slot A is live. Build 3 DISTINCT LUTs: the third must
        // recycle A's slot (A is LRU by then). A's handle -> stale_handle.
        for (int i = 0; i < 3; ++i) {
            std::vector<int64_t> k{100 + i}, v{200 + i};
            xi_pack_handle req = build_req(pk, k, v);
            xi_pack_handle out = XI_PACK_NULL;
            CHECK(cap->call("demo.lut.build", req, &out) == XI_CAP_OK);
            CHECK(pi64(pk, out, "built", -1) == 1);
            pk->release(out); pk->release(req);
        }
        const std::vector<int64_t> q{10};
        xi_pack_handle req = handle_req(pk, hA, &q);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("demo.lut.query", req, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "stale_handle");
        pk->release(out); pk->release(req);
        std::string s = lut->exchange("{\"command\":\"stats\"}");
        CHECK(jint(s, "recycles") >= 1);
        CHECK(jint(s, "stale_faults") >= 1);
        CHECK(jint(s, "live") == 3);
    }

    SECTION("recycle_all: the operator lever stales EVERY outstanding lease; "
            "a rebuilt LUT gets a FRESH generation (no aliasing)");
    {
        // Rebuild A's content (its slot was recycled above) and grab the lease.
        xi_pack_handle req = build_req(pk, K, V);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("demo.lut.build", req, &out) == XI_CAP_OK);
        CHECK(pi64(pk, out, "built", -1) == 1);               // real rebuild
        std::vector<uint8_t> hB = pmp(pk, out, "handle");
        Handle hb = parse_handle(hB);
        Handle ha = parse_handle(hA);
        CHECK(hb.ok && hb.gen > ha.gen);                      // generations never reuse
        pk->release(out); pk->release(req);

        std::string r = lut->exchange("{\"command\":\"recycle_all\"}");
        CHECK(jint(r, "recycled") >= 1);

        req = handle_req(pk, hB, nullptr);
        CHECK(cap->call("demo.lut.dump", req, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "stale_handle");
        pk->release(out); pk->release(req);

        // Same content again: a fresh build (dedup map was cleared), a fresh
        // handle, and the SAME byte-deterministic dump as before the recycle.
        req = build_req(pk, K, V);
        CHECK(cap->call("demo.lut.build", req, &out) == XI_CAP_OK);
        CHECK(pi64(pk, out, "built", -1) == 1);
        std::vector<uint8_t> hC = pmp(pk, out, "handle");
        pk->release(out); pk->release(req);
        req = handle_req(pk, hC, nullptr);
        CHECK(cap->call("demo.lut.dump", req, &out) == XI_CAP_OK);
        CHECK(pbin(pk, out, "lut") == dump1);                 // content-determined
        pk->release(out); pk->release(req);
    }

    SECTION("$probe answers the supported versions; unsupported request $v is a $fault pack");
    {
        xi_pack_builder b = pk->builder_new();
        if (pk->builder_add_bool) pk->builder_add_bool(b, "$probe", 1);
        else                      pk->builder_add_i64(b, "$probe", 1);
        xi_pack_handle preq = pk->builder_seal(b);
        for (const char* name : {"demo.lut.build", "demo.lut.query", "demo.lut.dump"}) {
            xi_pack_handle out = XI_PACK_NULL;
            CHECK(cap->call(name, preq, &out) == XI_CAP_OK);
            CHECK(pstr(pk, out, "$versions") == "1");
            pk->release(out);
        }
        pk->release(preq);

        b = pk->builder_new();
        pk->builder_add_i64(b, "$v", 7);
        xi_pack_handle vreq = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("demo.lut.build", vreq, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "unsupported_version");
        CHECK(pstr(pk, out, "$versions") == "1");
        pk->release(out); pk->release(vreq);
    }

    SECTION("contract fail-loud: missing/bad inputs answer $fault packs (rc stays 0)");
    {
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", 1);                       // no keys/values
        xi_pack_handle bad = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("demo.lut.build", bad, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "missing_input");
        pk->release(out); pk->release(bad);

        bad = build_req(pk, {1, 2}, {5});                     // length mismatch
        CHECK(cap->call("demo.lut.build", bad, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "bad_input");
        pk->release(out); pk->release(bad);

        bad = build_req(pk, {1, 1}, {5, 6});                  // duplicate keys
        CHECK(cap->call("demo.lut.build", bad, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "bad_input");
        pk->release(out); pk->release(bad);

        b = pk->builder_new();                                 // query without handle
        pk->builder_add_i64(b, "x", 1);
        bad = pk->builder_seal(b);
        CHECK(cap->call("demo.lut.query", bad, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "missing_input");
        pk->release(out); pk->release(bad);
    }

    SECTION("destroy: the lib unregisters (and the owner sweep backstops); "
            "the ring's objects die inside the DLL");
    {
        xi::InstanceRegistry::instance().remove("lut");
        lut.reset();
        CHECK(cap->available("demo.lut.build") == 0);
        CHECK(cap->available("demo.lut.query") == 0);
        CHECK(cap->available("demo.lut.dump")  == 0);
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", 1);
        xi_pack_handle in = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("demo.lut.query", in, &out) == XI_CAP_EUNKNOWN);
        pk->release(in);
    }

    SECTION("pack-registry balance");
    CHECK(xi::PackRegistry::instance().live_frames() == frames_baseline);

    if (g_failures == 0) {
        std::printf("\n[test] cap_lut_owner: ALL OK\n");
        return 0;
    }
    std::fprintf(stderr, "\n[test] cap_lut_owner: %d FAILURE(S)\n", g_failures);
    return 1;
}
