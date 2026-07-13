//
// test_script_pack.cpp — script-side Pack building (polaris2 gate P2,
// xi_script_pack.hpp).
//
// Covers:
//   1. Fail-closed pre-wiring — no host_api injected (or no xi.pack@1): the
//      builder is invalid, every add_* returns false, seal() yields an empty
//      ScriptPack. Never a crash — the t.pack() absent-pack contract.
//   2. Build-in-script -> seal -> read: every entry kind (i64/f64/str/bin/
//      bool/mp/image) added through ScriptPackBuilder reads back through the
//      SAME ScriptPack surface t.pack() yields, including the generic
//      for_each walk in insertion order.
//   3. CANONICAL BYTES — the load-bearing gate-P2 check. For each inline
//      entry, the pack's stored arena bytes (Pack::raw_at, reachable here
//      because the test is host-role and can dereference the registry) are
//      compared BYTE-FOR-BYTE against xi::mp::Writer encoding the same
//      logical value — ints 0xd3, floats 0xcb, str32/bin32 headers, canonical
//      bool, nested map/array in max-width profile.
//   4. Canonical-profile enforcement on nested values — non-canonical (but
//      valid) msgpack given to add_mp is RE-ENCODED to the canonical profile;
//      malformed bytes and ext-bearing values are REFUSED (add_mp false, no
//      entry); Writer::raw_canonical misuse cannot smuggle narrow widths in.
//   5. Lifecycle — seal consumes the builder (post-seal adds fail, double
//      seal empty); ScriptPack copies share the pack and the LAST drop
//      releases it (registry frames + ImagePool balance to baseline);
//      abandoning / dropping an unsealed builder leaks nothing.
//
// This TU plays the HOST ROLE for the script SDK headers (the
// test_parallel_safety.cpp idiom): it OWNS the storage for the extern globals
// xi_use.hpp declares (the script DLL defines them via the force-included
// xi_script_support.hpp) and injects the real host_api exactly as
// xi_script_loader does at DLL load.
//
#include <xi/xi_use.hpp>          // ScriptPack + the extern global decls
#include <xi/xi_script_pack.hpp>  // ScriptPackBuilder (the surface under test)
#include <xi/xi_pack_abi.hpp>     // install_pack_abi + PackRegistry (host role)
#include <xi/xi_image_pool.hpp>   // make_host_api + pool balance oracle
#include <xi/xi_mp.hpp>           // Writer (byte oracle) + Reader (decode)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

// xi_use.hpp declares these extern (the script DLL defines them static via
// xi_script_support.hpp). This host-role test owns the storage; only
// g_use_host_api_ is exercised here.
void* g_use_exchange_fn_   = nullptr;
void* g_use_host_api_      = nullptr;
void* g_trigger_info_fn_   = nullptr;
void* g_trigger_image_fn_  = nullptr;
void* g_trigger_sources_fn_ = nullptr;
void* g_trigger_leader_fn_  = nullptr;

static int pool_live() { return xi::ImagePool::instance().cumulative().live_now; }

// Byte-compare a pack entry's CANONICAL wire bytes against an expected
// canonical encoding. Host-role: dereference the registry and emit through the
// slab pack's canonical walk (Pack::canonical_value) — the slab stores scalars
// raw, so canonical parity is now proven at the walk seam, not raw_at.
static bool raw_equals(const xi::ScriptPack& sp, size_t idx,
                       const uint8_t* want, size_t want_n) {
    xi::Pack* p = xi::PackRegistry::instance().pack(sp.handle());
    if (!p || idx >= p->size()) return false;
    xi::mp::Writer w;
    if (!p->canonical_value(idx, w)) return false;
    return w.size() == want_n &&
           (want_n == 0 || std::memcmp(w.bytes().data(), want, want_n) == 0);
}

// ---------------------------------------------------------------------------
// (1) Fail-closed before the host wires anything.
// ---------------------------------------------------------------------------
static void test_fail_closed_unwired() {
    SECTION("no host_api injected -> invalid builder, adds false, seal empty");
    g_use_host_api_ = nullptr;

    xi::ScriptPackBuilder b;
    CHECK(!b.valid());
    CHECK(!b);
    CHECK(!b.add_i64("seq", 1));
    CHECK(!b.add_str("label", "x"));
    CHECK(!b.add_bool("pass", true));
    xi::mp::Writer w; w.int_(1);
    CHECK(!b.add_mp("v", w));

    xi::ScriptPack sp = b.seal();
    CHECK(!sp.valid());
    CHECK(sp.count() == 0);
    CHECK(!sp.get_i64("seq"));
}

// ---------------------------------------------------------------------------
// (2)+(3) Build in script land, read back, and assert canonical bytes.
// ---------------------------------------------------------------------------
static void test_build_read_canonical() {
    SECTION("ScriptPackBuilder -> seal -> ScriptPack reads + canonical bytes");
    size_t base_frames = xi::PackRegistry::instance().live_frames();
    int    base_pool   = pool_live();
    {
        xi::ScriptPackBuilder b;
        CHECK(b.valid());

        // The nested canonical value: [ {"x":3, "y":4} ] via the canonical Writer.
        xi::mp::Writer pts;
        pts.array(1); pts.map(2);
        pts.key("x"); pts.int_(3);
        pts.key("y"); pts.int_(4);

        std::vector<uint8_t> gray(16, 0);
        gray[5] = 200; gray[6] = 210;
        const uint8_t blob[3] = {1, 2, 3};

        CHECK(b.add_i64("seq", 42));
        CHECK(b.add_f64("score", 2.5));
        CHECK(b.add_str("label", "part-A"));
        CHECK(b.add_bin("raw", blob, 3));
        CHECK(b.add_bool("pass", true));
        CHECK(b.add_mp("pts", pts));
        CHECK(b.add_image("gray", 4, 4, 1, gray.data()));

        xi::ScriptPack sp = b.seal();
        CHECK(sp.valid());
        CHECK(!b.valid());          // seal consumed the builder
        CHECK(sp.count() == 7);

        // ---- reads through the exact surface t.pack() yields ----
        CHECK(sp.get_i64("seq").value_or(-1) == 42);
        CHECK(sp.get_f64("score").value_or(0) == 2.5);
        CHECK(sp.get_str("label").value_or("") == "part-A");
        auto bin = sp.get_bin("raw");
        CHECK(bin && bin->size() == 3 && (*bin)[2] == 3);
        auto img = sp.get_image("gray");
        CHECK(img && img->width == 4 && img->height == 4 && img->channels == 1);
        CHECK(img && img->pixels.size() == 16 && img->pixels[6] == 210);
        CHECK(sp.tag_of("seq") == XI_PACK_TAG_I64);
        CHECK(sp.tag_of("gray") == XI_PACK_TAG_IMAGE);
        CHECK(sp.tag_of("pass") == XI_PACK_TAG_BOOL);   // native bool entry

        // Bool reads back through the typed accessor (native tail).
        CHECK(sp.get_bool("pass").value_or(false) == true);
        CHECK(!sp.get_mp("pass"));   // fail-closed: a bool entry is NOT an Mp entry
        // Nested value round-trips structurally.
        auto mp = sp.get_mp("pts");
        CHECK(mp.has_value());
        if (mp) {
            xi::mp::Reader r(mp->data(), mp->size());
            xi::mp::Element e;
            CHECK(r.next(e) == xi::mp::Status::Ok && e.kind == xi::mp::Kind::Array && e.len == 1);
            CHECK(r.next(e) == xi::mp::Status::Ok && e.kind == xi::mp::Kind::Map && e.len == 2);
        }

        // Generic walk: insertion order + tags (the expose/record_save path).
        std::vector<std::string> keys;
        std::vector<int32_t> tags;
        sp.for_each([&](std::string_view k, int32_t t) {
            keys.emplace_back(k);
            tags.push_back(t);
        });
        CHECK(keys.size() == 7);
        CHECK(keys[0] == "seq" && keys[1] == "score" && keys[2] == "label" &&
              keys[3] == "raw" && keys[4] == "pass" && keys[5] == "pts" &&
              keys[6] == "gray");
        CHECK(tags[0] == XI_PACK_TAG_I64  && tags[1] == XI_PACK_TAG_F64 &&
              tags[2] == XI_PACK_TAG_STR  && tags[3] == XI_PACK_TAG_BIN &&
              tags[4] == XI_PACK_TAG_BOOL && tags[5] == XI_PACK_TAG_MP &&
              tags[6] == XI_PACK_TAG_IMAGE);

        // ---- (3) canonical bytes: stored arena bytes == xi::mp::Writer ----
        { xi::mp::Writer w; w.int_(42);
          CHECK(raw_equals(sp, 0, w.bytes().data(), w.bytes().size()));
          CHECK(w.bytes().size() == 9 && w.bytes()[0] == 0xd3); }   // int64 always
        { xi::mp::Writer w; w.float_(2.5);
          CHECK(raw_equals(sp, 1, w.bytes().data(), w.bytes().size()));
          CHECK(w.bytes().size() == 9 && w.bytes()[0] == 0xcb); }   // float64 always
        { xi::mp::Writer w; w.str("part-A");
          CHECK(raw_equals(sp, 2, w.bytes().data(), w.bytes().size()));
          CHECK(w.bytes()[0] == 0xdb); }                            // str32 always
        { xi::mp::Writer w; w.bin(blob, 3);
          CHECK(raw_equals(sp, 3, w.bytes().data(), w.bytes().size()));
          CHECK(w.bytes()[0] == 0xc6); }                            // bin32 always
        { xi::mp::Writer w; w.boolean(true);
          CHECK(raw_equals(sp, 4, w.bytes().data(), w.bytes().size())); }
        // The nested value landed byte-identical to the Writer's canonical
        // bytes (canonicalize is the identity on canonical input).
        CHECK(raw_equals(sp, 5, pts.bytes().data(), pts.bytes().size()));

        // Copies are cheap and share the pack (both read; still one frame).
        xi::ScriptPack sp2 = sp;
        CHECK(sp2.get_i64("seq").value_or(-1) == 42);
        CHECK(xi::PackRegistry::instance().live_frames() == base_frames + 1);
    }
    // Everything dropped: pack released, image pool + registry at baseline.
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames);
    CHECK(pool_live() == base_pool);
}

// ---------------------------------------------------------------------------
// (4) Canonical-profile enforcement on nested bytes (add_mp is the gate).
// ---------------------------------------------------------------------------
static void test_canonical_enforcement() {
    SECTION("add_mp: non-canonical re-encoded; malformed/ext refused");
    xi::ScriptPackBuilder b;
    CHECK(b.valid());

    // (a) VALID but NON-CANONICAL msgpack: 42 as a positive fixint (one byte).
    //     A stock encoder would emit this; our profile demands int64 0xd3.
    //     The gate accepts it and RE-ENCODES to canonical max-width.
    const uint8_t fixint42[1] = {0x2a};
    CHECK(b.add_mp("narrow", fixint42, 1));

    // (b) Writer::raw_canonical misuse: splicing narrow bytes into a Writer
    //     does not smuggle them past the gate either — same re-encode.
    xi::mp::Writer smuggle;
    smuggle.raw_canonical(fixint42, 1);
    CHECK(b.add_mp("smuggled", smuggle));

    // (c) MALFORMED bytes: the reserved byte 0xc1 -> refused outright.
    const uint8_t reserved[1] = {0xc1};
    CHECK(!b.add_mp("bad", reserved, 1));

    // (d) TRUNCATED value: a str32 header promising 100 bytes with none.
    const uint8_t truncated[5] = {0xdb, 0x00, 0x00, 0x00, 0x64};
    CHECK(!b.add_mp("trunc", truncated, 5));

    // (e) EXT smuggle (a forged pool-handle would ride ext): reject-all policy.
    xi::mp::Writer evil;
    const uint8_t payload[4] = {1, 2, 3, 4};
    evil.ext_privileged(7, payload, 4);
    CHECK(!b.add_mp("ext", evil));

    // (f) Empty / null input is refused (an Mp entry is ONE complete value).
    CHECK(!b.add_mp("empty", nullptr, 0));

    xi::ScriptPack sp = b.seal();
    CHECK(sp.valid());
    CHECK(sp.count() == 2);            // only the two re-encoded entries landed
    CHECK(!sp.get_mp("bad") && !sp.get_mp("trunc") && !sp.get_mp("ext") &&
          !sp.get_mp("empty"));

    // Both accepted entries hold the CANONICAL encoding of 42 — 9 bytes 0xd3,
    // byte-identical to the Writer — not the fixint they were given.
    xi::mp::Writer want; want.int_(42);
    CHECK(raw_equals(sp, 0, want.bytes().data(), want.bytes().size()));
    CHECK(raw_equals(sp, 1, want.bytes().data(), want.bytes().size()));
    auto narrow = sp.get_mp("narrow");
    CHECK(narrow && narrow->size() == 9 && (*narrow)[0] == 0xd3);
}

// ---------------------------------------------------------------------------
// (5) Lifecycle: seal-consumes, abandon, refcount balance.
// ---------------------------------------------------------------------------
static void test_lifecycle() {
    SECTION("seal consumes; post-seal adds fail; abandon/drop leak nothing");
    size_t base_frames   = xi::PackRegistry::instance().live_frames();
    size_t base_builders = xi::PackRegistry::instance().live_builders();
    int    base_pool     = pool_live();

    // Post-seal adds fail; double seal is empty.
    xi::ScriptPackBuilder b;
    CHECK(b.add_i64("n", 7));
    xi::ScriptPack sp = b.seal();
    CHECK(sp.valid());
    CHECK(!b.add_i64("late", 1));      // the builder is dead
    CHECK(!b.seal().valid());          // double seal -> empty, no crash

    // The last ScriptPack copy releases the pack.
    {
        xi::ScriptPack sp2 = sp;
        sp = xi::ScriptPack{};         // drop the first ref; sp2 still owns it
        CHECK(sp2.get_i64("n").value_or(-1) == 7);
        CHECK(xi::PackRegistry::instance().live_frames() == base_frames + 1);
    }
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames);

    // A builder dropped without seal abandons host-side (image handle freed).
    {
        std::vector<uint8_t> px(16, 1);
        xi::ScriptPackBuilder b2;
        CHECK(b2.add_image("frame", 4, 4, 1, px.data()));
        CHECK(pool_live() == base_pool + 1);
        // scope exit -> ~ScriptPackBuilder -> builder_abandon
    }
    CHECK(pool_live() == base_pool);
    CHECK(xi::PackRegistry::instance().live_builders() == base_builders);

    // Explicit abandon is idempotent and invalidates.
    xi::ScriptPackBuilder b3;
    CHECK(b3.valid());
    b3.abandon();
    CHECK(!b3.valid());
    b3.abandon();                      // second abandon: harmless
    CHECK(!b3.add_i64("x", 1));

    // Move transfers ownership exactly once.
    xi::ScriptPackBuilder b4;
    CHECK(b4.add_str("k", "v"));
    xi::ScriptPackBuilder b5 = std::move(b4);
    CHECK(!b4.valid());
    CHECK(b5.valid());
    xi::ScriptPack mp5 = b5.seal();
    CHECK(mp5.valid() && mp5.get_str("k").value_or("") == "v");

    CHECK(xi::PackRegistry::instance().live_builders() == base_builders);
}

int main() {
    std::printf("[test] script-side Pack building (gate P2, xi_script_pack.hpp)\n");

    // (1) runs BEFORE the host wires anything — the pre-injection reality.
    test_fail_closed_unwired();

    // Host role: publish the pack plane and inject the host_api, exactly as
    // the backend does for a loaded script DLL (xi_script_set_use_callbacks).
    xi::install_pack_abi();
    static xi_host_api api = xi::ImagePool::make_host_api();
    g_use_host_api_ = (void*)&api;

    test_build_read_canonical();
    test_canonical_enforcement();
    test_lifecycle();

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
