// test_xi_pack.cpp — unit tests for the keyed-buffer Pack container
// (xi_pack.hpp), SELF-DESCRIBING BLOB plane (spec 30). Covers the pack
// lifecycle (produce -> seal -> borrow -> drop), O(1) offset-index correctness
// at scale, immutability/seal semantics, pooled-handle balance verified against
// ImagePool's own stats, mixed small/large packs, the drop-on-crash story, and
// the blob surface: mint/adopt/get round-trip with 64B payload alignment, the
// blob_head_validate rejection matrix, type_of, image-as-convention, the uint32
// seal guard, and sort_idx recycle sanity.

#include "xi/xi_pack.hpp"
#include "xi/xi_image_pool.hpp"

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

using xi::Pack;
using xi::PackBuilder;
using xi::PackTag;
using xi::BufRef;
using xi::BlobView;

static int pool_live() { return xi::ImagePool::instance().cumulative().live_now; }

// ------------------------------------------------------------------
// The _keys.h-style contract layer (doc 02), applied to Pack.
// ------------------------------------------------------------------
namespace blob_keys {
inline constexpr std::string_view kThreshold = "threshold";
inline constexpr std::string_view kBlobCount = "blob_count";
inline constexpr std::string_view kMeanArea  = "mean_area";
inline constexpr std::string_view kLabel     = "label";
} // namespace blob_keys

struct BlobResult {
    int64_t threshold = 0;
    int64_t blob_count = 0;
    double  mean_area = 0.0;
    std::string label;
};

static void build_blob(PackBuilder& b, const BlobResult& r) {
    b.add_i64(blob_keys::kThreshold, r.threshold);
    b.add_i64(blob_keys::kBlobCount, r.blob_count);
    b.add_f64(blob_keys::kMeanArea,  r.mean_area);
    b.add_str(blob_keys::kLabel,     r.label);
}
static bool extract_blob(const Pack& f, BlobResult& out) {
    auto th = f.get_i64(blob_keys::kThreshold);
    auto bc = f.get_i64(blob_keys::kBlobCount);
    auto ma = f.get_f64(blob_keys::kMeanArea);
    auto lb = f.get_str(blob_keys::kLabel);
    if (!th || !bc || !ma || !lb) return false;
    out.threshold = *th; out.blob_count = *bc; out.mean_area = *ma;
    out.label = std::string(*lb);
    return true;
}

// ------------------------------------------------------------------
static void test_lifecycle_and_contract_layer() {
    BlobResult in{128, 7, 42.5, "pass"};
    PackBuilder b;
    build_blob(b, in);
    CHECK(!b.sealed(), "builder not sealed pre-seal");

    Pack f = b.seal();
    CHECK(b.sealed(), "builder sealed after seal");
    CHECK(f.size() == 4, "pack has 4 entries");
    CHECK(f.has(blob_keys::kThreshold), "has threshold");
    CHECK(!f.has("nonexistent"), "missing key absent");
    CHECK(f.tag_of(blob_keys::kMeanArea) == PackTag::F64, "mean_area is f64");

    BlobResult out;
    CHECK(extract_blob(f, out), "extract succeeds");
    CHECK(out.threshold == 128, "threshold round-trips");
    CHECK(out.blob_count == 7, "blob_count round-trips");
    CHECK(out.mean_area == 42.5, "mean_area round-trips");
    CHECK(out.label == "pass", "label round-trips");

    CHECK(f.get<int64_t>(blob_keys::kThreshold).value() == 128, "get<i64>");
    CHECK(f.get<double>(blob_keys::kMeanArea).value() == 42.5, "get<f64>");

    CHECK(!f.get_str(blob_keys::kThreshold).has_value(), "type-mismatch read is nullopt");
    CHECK(!f.get_i64("missing").has_value(), "missing-key read is nullopt");
}

// ------------------------------------------------------------------
static void test_bool_entry() {
    PackBuilder b;
    b.add_bool("pass", true);
    b.add_bool("fail", false);
    b.add_i64("one", 1);
    Pack f = b.seal();
    CHECK(f.tag_of("pass") == PackTag::Bool, "bool entry stores the Bool tag");
    CHECK(f.get_bool("pass").value() == true,  "get_bool true round-trips");
    CHECK(f.get_bool("fail").value() == false, "get_bool false round-trips");
    CHECK(f.get<bool>("pass").value() == true, "get<bool> alias");
    CHECK(!f.get_i64("pass").has_value(), "bool entry refuses an i64 read");
    CHECK(!f.get_bool("one").has_value(), "i64 entry refuses a bool read");
    CHECK(f.raw_at(0).size() == 1 && f.raw_at(0)[0] == 0xc3, "raw true byte 0xc3");
    CHECK(f.raw_at(1).size() == 1 && f.raw_at(1)[0] == 0xc2, "raw false byte 0xc2");
    {
        xi::mp::Writer w;
        CHECK(f.canonical_value(0, w) && f.canonical_value(1, w), "canonical_value emits bools");
        CHECK(w.size() == 2 && w.bytes()[0] == 0xc3 && w.bytes()[1] == 0xc2,
              "canonical walk emits 0xc3/0xc2 (== raw_at, wire parity)");
    }
}

// Insertion-ordered walk + O(1) index correctness at scale.
static void test_offset_index_at_scale() {
    const int N = 2000;
    PackBuilder b;
    for (int i = 0; i < N; ++i)
        b.add_i64("k" + std::to_string(i), int64_t(i) * 3 + 1);
    Pack f = b.seal();
    CHECK(f.size() == size_t(N), "all N entries present");

    bool all = true;
    for (int i = N - 1; i >= 0; --i) {
        auto v = f.get_i64("k" + std::to_string(i));
        if (!v || *v != int64_t(i) * 3 + 1) { all = false; break; }
    }
    CHECK(all, "every key resolves to its value after many inserts");

    int seen = 0; bool ordered = true;
    f.for_each([&](std::string_view key, PackTag) {
        if (key != std::string("k" + std::to_string(seen))) ordered = false;
        ++seen;
    });
    CHECK(seen == N && ordered, "for_each visits every entry in insertion order");
}

static void test_seal_semantics() {
    PackBuilder b;
    b.add_i64("x", 1);
    CHECK(!b.sealed(), "not sealed before seal()");
    Pack f = b.seal();
    CHECK(b.sealed(), "sealed flag set — further add_* would assert");
    CHECK(f.get_i64("x").value() == 1, "sealed pack still reads");
}

// Duplicate-key behaviour: first-inserted wins on lookup; both entries survive
// the walk (unchanged from the arena/slab container).
static void test_duplicate_key() {
    PackBuilder b;
    b.add_i64("dup", 111);
    b.add_str("dup", "second");
    b.add_i64("other", 9);
    Pack f = b.seal();
    CHECK(f.size() == 3, "both duplicate entries present");
    // find() returns the FIRST-inserted entry, so tag_of/get resolve to the i64.
    CHECK(f.tag_of("dup") == PackTag::I64, "duplicate lookup resolves first-inserted (i64)");
    CHECK(f.get_i64("dup").value() == 111, "first-inserted value wins");
    CHECK(!f.get_str("dup").has_value(), "second-inserted (str) is shadowed on lookup");
    // The walk still visits BOTH in insertion order.
    int dups = 0;
    f.for_each([&](std::string_view k, PackTag) { if (k == "dup") ++dups; });
    CHECK(dups == 2, "insertion walk visits both duplicate entries");
}

// ------------------------------------------------------------------
// Blob head round-trip: mint -> fill payload in place -> adopt -> get.
// Verifies the descriptor view round-trips AND the payload is 64B-aligned.
// ------------------------------------------------------------------
static void test_blob_roundtrip() {
    const int base = pool_live();
    // A custom convention type with arbitrary keys the core never interprets.
    xi::mp::Bytes desc = xi::BlobDesc("acme/profile3d")
        .i64("rows", 3).i64("cols", 5).str("units", "mm").build();
    const int64_t payload_len = 3 * 5 * sizeof(double);
    {
        BufRef ref = xi::mint_blob(desc.data(), int32_t(desc.size()), payload_len);
        CHECK((bool)ref, "mint_blob succeeds for a canonical descriptor");
        CHECK(ref.payload_len() == payload_len, "BufRef exposes the payload length");
        CHECK((reinterpret_cast<uintptr_t>(ref.payload()) & 63u) == 0,
              "minted payload region is 64B-aligned");
        // Fill the payload in place (the zero-copy producer pattern).
        auto* px = reinterpret_cast<double*>(ref.payload());
        for (int i = 0; i < 15; ++i) px[i] = double(i) + 0.25;

        PackBuilder b;
        b.add_i64("seq", 1);
        CHECK(b.adopt_blob("surf", ref), "adopt_blob succeeds");
        Pack f = b.seal();
        // The BufRef still holds its mint ref here; it drops at scope exit so the
        // pack co-owns during its life. Handle count = 1 (the blob).
        CHECK(f.handle_count() == 1, "pack owns 1 pool handle (the blob)");
        CHECK(f.tag_of("surf") == PackTag::Blob, "blob entry stores the Blob tag");

        auto bv = f.get_blob("surf");
        CHECK(bv.has_value(), "get_blob returns a view");
        CHECK(bv && bv->desc.size() == desc.size() &&
              std::memcmp(bv->desc.data(), desc.data(), desc.size()) == 0,
              "descriptor view round-trips byte-identically");
        CHECK(bv && bv->payload_len == payload_len, "payload_len round-trips");
        CHECK(bv && (reinterpret_cast<uintptr_t>(bv->payload.data()) & 63u) == 0,
              "get_blob payload span is 64B-aligned");
        bool px_ok = bv.has_value();
        if (bv) {
            auto* rp = reinterpret_cast<const double*>(bv->payload.data());
            for (int i = 0; i < 15; ++i) px_ok = px_ok && (rp[i] == double(i) + 0.25);
        }
        CHECK(px_ok, "payload bytes round-trip through the pool buffer");

        // Convention sugar: type_of reads "t"; the core never interprets others.
        auto t = f.type_of("surf");
        CHECK(t && *t == "acme/profile3d", "type_of reads the convention type string");
        CHECK(!f.type_of("seq").has_value(), "type_of on a non-blob is nullopt");
        // Descriptor field reads (the SDK accessor path).
        CHECK(Pack::desc_find_i64(bv->desc, "rows").value_or(-1) == 3, "desc_find_i64 rows");
        CHECK(Pack::desc_find_str(bv->desc, "units").value_or("") == "mm", "desc_find_str units");
    }
    // BufRef dropped + pack dropped -> pool balanced.
    CHECK(pool_live() == base, "blob pool handle released when pack + BufRef drop");
}

// add_blob (mint + copy convenience): the caller has the payload in hand.
static void test_add_blob_copy() {
    const int base = pool_live();
    xi::mp::Bytes desc = xi::BlobDesc("acme/scan").i64("n", 4).build();
    uint32_t payload[4] = {10, 20, 30, 40};
    {
        PackBuilder b;
        CHECK(b.add_blob("s", desc.data(), int32_t(desc.size()),
                         payload, sizeof payload),
              "add_blob copies payload into a fresh buffer");
        Pack f = b.seal();
        CHECK(f.handle_count() == 1, "add_blob pack owns 1 handle");
        auto bv = f.get_blob("s");
        CHECK(bv && bv->payload_len == int64_t(sizeof payload), "add_blob payload_len");
        bool ok = bv.has_value();
        if (bv) {
            auto* p = reinterpret_cast<const uint32_t*>(bv->payload.data());
            for (int i = 0; i < 4; ++i) ok = ok && (p[i] == payload[i]);
        }
        CHECK(ok, "add_blob payload bytes intact");
    }
    CHECK(pool_live() == base, "add_blob pack balances the pool on drop");
}

// image-as-convention: mint_image builds {"t":"xi/image","w","h","c","dt"} and
// mints a blob; the descriptor carries the shape, the payload the pixels.
static void test_image_as_convention() {
    const int base = pool_live();
    {
        BufRef ref = xi::mint_image(8, 4, 3, "u8");
        CHECK((bool)ref, "mint_image succeeds");
        CHECK(ref.payload_len() == 8 * 4 * 3, "mint_image sizes payload w*h*c*elem");
        std::memset(ref.payload(), 0x5A, size_t(ref.payload_len()));

        PackBuilder b;
        CHECK(b.adopt_blob("frame", ref), "adopt image blob");
        Pack f = b.seal();
        auto bv = f.get_blob("frame");
        CHECK(bv.has_value(), "image blob reads back");
        CHECK(f.type_of("frame").value_or("") == "xi/image", "image blob type is xi/image");
        CHECK(bv && Pack::desc_find_i64(bv->desc, "w").value_or(-1) == 8, "w from descriptor");
        CHECK(bv && Pack::desc_find_i64(bv->desc, "h").value_or(-1) == 4, "h from descriptor");
        CHECK(bv && Pack::desc_find_i64(bv->desc, "c").value_or(-1) == 3, "c from descriptor");
        CHECK(bv && Pack::desc_find_str(bv->desc, "dt").value_or("") == "u8", "dt from descriptor");
        CHECK(bv && bv->payload.size() == 8u * 4u * 3u && bv->payload[0] == 0x5A,
              "image pixels ride the blob payload");
    }
    CHECK(pool_live() == base, "image blob balances the pool on drop");
    // Unknown dtype -> empty BufRef (fail-closed).
    CHECK(!(bool)xi::mint_image(4, 4, 1, "float128"), "mint_image rejects unknown dtype");
}

// ------------------------------------------------------------------
// blob_head_validate rejection matrix (spec 30 fail-loud seam).
// ------------------------------------------------------------------
static void test_blob_head_validate_matrix() {
    // A known-good buffer: 'XBD1' + desc_len + canonical map + pad + payload.
    xi::mp::Bytes desc = xi::BlobDesc("t/x").i64("a", 1).build();
    const uint32_t dlen = uint32_t(desc.size());
    const uint64_t poff = xi::blob_payload_off(dlen);
    const size_t total = size_t(poff) + 16;
    std::vector<uint8_t> good(total, 0);
    xi::pack_mp_detail::put_u32_le(good.data() + 0, xi::kBlobMagic);
    xi::pack_mp_detail::put_u32_le(good.data() + 4, dlen);
    std::memcpy(good.data() + 8, desc.data(), dlen);
    CHECK(xi::blob_head_validate(good.data(), good.size()), "valid blob head accepts");

    // 1) bad magic.
    {
        std::vector<uint8_t> bad = good;
        xi::pack_mp_detail::put_u32_le(bad.data() + 0, 0xDEADBEEFu);
        CHECK(!xi::blob_head_validate(bad.data(), bad.size()), "bad magic rejected");
    }
    // 2) desc_len overrun (declares more descriptor than the buffer holds).
    {
        std::vector<uint8_t> bad = good;
        xi::pack_mp_detail::put_u32_le(bad.data() + 4, uint32_t(bad.size()));  // 8+dlen > len
        CHECK(!xi::blob_head_validate(bad.data(), bad.size()), "desc_len overrun rejected");
    }
    // 3) non-canonical descriptor (corrupt the map bytes so canonicalize fails /
    //    the top element is no longer a well-formed map).
    {
        std::vector<uint8_t> bad = good;
        bad[8] = 0xc1;  // 0xc1 is the msgpack never-used / reserved byte
        CHECK(!xi::blob_head_validate(bad.data(), bad.size()), "non-canonical descriptor rejected");
    }
    // 3b) a descriptor that is a valid msgpack SCALAR, not a map, is rejected.
    {
        xi::mp::Writer w; w.int_(7);
        xi::mp::Bytes sdesc = w.take();
        const uint32_t sl = uint32_t(sdesc.size());
        const size_t st = size_t(xi::blob_payload_off(sl)) + 8;
        std::vector<uint8_t> bad(st, 0);
        xi::pack_mp_detail::put_u32_le(bad.data() + 0, xi::kBlobMagic);
        xi::pack_mp_detail::put_u32_le(bad.data() + 4, sl);
        std::memcpy(bad.data() + 8, sdesc.data(), sl);
        CHECK(!xi::blob_head_validate(bad.data(), bad.size()), "non-map descriptor rejected");
    }
    // 4) payload_off > len (buffer truncated below the aligned payload offset).
    {
        // Truncate to just past the descriptor but before payload_off.
        size_t trunc = size_t(8 + dlen);
        CHECK(trunc < poff, "descriptor ends before the aligned payload offset");
        CHECK(!xi::blob_head_validate(good.data(), trunc), "payload_off > len rejected");
    }
    // 5) too short to even hold the head.
    CHECK(!xi::blob_head_validate(good.data(), 4), "sub-head length rejected");
    CHECK(!xi::blob_head_validate(nullptr, 0), "null base rejected");

    // adopt_blob refuses a non-blob pooled buffer (fail-loud through the seam).
    {
        std::vector<uint8_t> junk(128, 0xAB);
        xi_image_handle h = xi::pack_pool::alloc_bytes(junk.data(), junk.size());
        CHECK(h != XI_IMAGE_NULL, "minted a junk (non-blob) pool buffer");
        PackBuilder b;
        CHECK(!b.adopt_blob("bad", h), "adopt_blob refuses a buffer with no valid head");
        // b abandons -> nothing adopted; release our own mint ref.
        xi::pack_pool::release(h);
    }
}

// alloc_bytes null-src hard reject (zeroinit verdict).
static void test_alloc_bytes_null_reject() {
    CHECK(xi::pack_pool::alloc_bytes(nullptr, 128) == XI_IMAGE_NULL,
          "alloc_bytes hard-rejects a null src (copy path with nothing to copy)");
    CHECK(xi::pack_pool::alloc_bytes(nullptr, 0) == XI_IMAGE_NULL,
          "alloc_bytes rejects zero length");
}

static void test_pooled_handle_balance() {
    const int base = pool_live();
    std::vector<uint8_t> big(8192, 0xCD);   // >= threshold -> pooled bin
    {
        PackBuilder b;
        b.add_i64("n", 1);
        b.add_bin("payload", big.data(), big.size());
        Pack f = b.seal();
        CHECK(f.handle_count() == 1, "pack owns 1 pool handle (big bin)");
        CHECK(pool_live() == base + 1, "pool live count rose by 1 while pack alive");
        auto bin = f.get_bin("payload");
        CHECK(bin && bin->size() == big.size() && (*bin)[0] == 0xCD,
              "large bin resolves through the pool");
    }
    CHECK(pool_live() == base, "all pooled handles released when pack dropped");
}

static void test_mixed_frame() {
    const int base = pool_live();
    uint8_t small_bin[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    xi::mp::Bytes desc = xi::BlobDesc("acme/x").i64("k", 2).build();
    uint16_t blobpx[4] = {1, 2, 3, 4};
    {
        PackBuilder b;
        b.add_i64("i", -99);
        b.add_f64("f", 3.14159);
        b.add_str("s", "hello pack");
        b.add_bin("tiny", small_bin, sizeof small_bin);   // inline, no handle
        b.add_blob("blob", desc.data(), int32_t(desc.size()), blobpx, sizeof blobpx);
        Pack f = b.seal();

        CHECK(f.handle_count() == 1, "only the blob is pooled; tiny bin is inline");
        CHECK(pool_live() == base + 1, "one pool handle live");
        CHECK(f.get_i64("i").value() == -99, "negative i64 round-trips");
        CHECK(f.get_f64("f").value() == 3.14159, "f64 round-trips");
        CHECK(f.get_str("s").value() == "hello pack", "str round-trips");
        auto tb = f.get_bin("tiny");
        CHECK(tb && tb->size() == 8 && (*tb)[7] == 8, "inline tiny bin round-trips");
        CHECK(f.tag_of("tiny") == PackTag::Bin, "tiny bin tagged Bin regardless of storage");
        auto bv = f.get_blob("blob");
        CHECK(bv && bv->payload.size() == sizeof blobpx, "blob payload reads");
    }
    CHECK(pool_live() == base, "mixed pack balances the pool on drop");
}

static void test_crash_drop_no_double_release() {
    const int base = pool_live();
    xi::mp::Bytes desc = xi::BlobDesc("acme/x").build();
    {
        Pack moved_to;   // empty
        {
            PackBuilder b;
            b.add_blob("m", desc.data(), int32_t(desc.size()), nullptr, 0);
            Pack f = b.seal();
            CHECK(pool_live() == base + 1, "one handle live after seal");
            moved_to = std::move(f);
        }
        CHECK(pool_live() == base + 1, "moved-from pack drop released nothing");
        CHECK(moved_to.handle_count() == 1, "ownership transferred to the move target");
    }
    CHECK(pool_live() == base, "the single live owner released exactly once");

    // A builder abandoned without seal() still releases what it adopted.
    {
        const int b2 = pool_live();
        {
            PackBuilder b;
            BufRef ref = xi::mint_blob(desc.data(), int32_t(desc.size()), 0);
            b.adopt_blob("x", ref);       // pack co-owns (rc2)
            CHECK(pool_live() == b2 + 1, "unsealed builder adopted a handle");
            // ref drops (rc1); builder destructs without seal -> releases its ref.
        }
        CHECK(pool_live() == b2, "unsealed builder + BufRef released the handle on drop");
    }
}

// ------------------------------------------------------------------
// Canonical-walk parity for inline scalar/str/bin/mp entries + Blob refusal.
// ------------------------------------------------------------------
static void test_canonical_walk_parity() {
    const double nan_payload = []{
        uint64_t bits = 0xfff800000000beefull;
        double d; std::memcpy(&d, &bits, sizeof d); return d;
    }();
    uint8_t bin[3] = {9, 8, 7};
    xi::mp::Writer nested;
    nested.map(1); nested.key("x"); nested.int_(4);

    PackBuilder b;
    b.add_i64("i", -12345);
    b.add_f64("f", 2.75);
    b.add_f64("nan", nan_payload);
    b.add_bool("t", true);
    b.add_str("s", "walkme");
    b.add_bin("bin", bin, sizeof bin);
    b.add_mp("m", nested.bytes().data(), nested.bytes().size());
    Pack f = b.seal();

    xi::mp::Writer want;
    want.int_(-12345);
    want.float_(2.75);
    want.float_(nan_payload);
    want.boolean(true);
    want.str("walkme");
    want.bin(bin, sizeof bin);
    want.raw_canonical(nested.bytes().data(), nested.bytes().size());

    xi::mp::Writer got;
    for (size_t i = 0; i < f.size(); ++i)
        CHECK(f.canonical_value(i, got), "canonical_value succeeds for every inline tag");
    CHECK(got.size() == want.size() &&
          std::memcmp(got.bytes().data(), want.bytes().data(), want.size()) == 0,
          "canonical walk is byte-identical to xi::mp::Writer (incl. NaN flatten)");

    uint64_t bits; double d = f.get_f64("nan").value();
    std::memcpy(&bits, &d, sizeof bits);
    CHECK(bits == 0x7ff8000000000000ull, "stored NaN is the canonical quiet pattern");

    // A Blob entry has no single canonical scalar value — the walk refuses it.
    xi::mp::Bytes desc = xi::BlobDesc("acme/x").build();
    PackBuilder b2;
    b2.add_blob("blob", desc.data(), int32_t(desc.size()), nullptr, 0);
    Pack f2 = b2.seal();
    xi::mp::Writer w2;
    CHECK(!f2.canonical_value(0, w2) && w2.size() == 0,
          "canonical_value refuses a blob entry, writer untouched");
}

// ------------------------------------------------------------------
// The entry-view walk: insertion order, typed detail, extern descriptor.
// ------------------------------------------------------------------
static void test_for_each_entry_walk() {
    const int base = pool_live();
    xi::mp::Bytes desc = xi::BlobDesc("acme/x").i64("n", 1).build();
    uint8_t px[12] = {0};
    {
        PackBuilder b;
        b.add_i64("first", 1);
        b.add_blob("blob", desc.data(), int32_t(desc.size()), px, sizeof px);
        b.add_str("last", "z");
        Pack f = b.seal();
        size_t seen = 0;
        f.for_each_entry([&](const Pack::EntryView& e) {
            if (e.ordinal == 0) {
                CHECK(e.key == "first" && e.tag == PackTag::I64 && !e.external,
                      "entry 0 is the inline i64");
                CHECK(e.raw.size() == 9 && e.raw[0] == 0xd3,
                      "raw i64 is the canonical int64 value (0xd3 + 8, ④A wire==memory)");
            } else if (e.ordinal == 1) {
                CHECK(e.key == "blob" && e.tag == PackTag::Blob && e.external,
                      "entry 1 is the extern blob");
                CHECK(e.ext_len == xi::blob_payload_off(uint32_t(desc.size())) + sizeof px,
                      "extern len is the whole self-describing buffer");
                CHECK(e.handle != XI_IMAGE_NULL, "extern view exposes the pool handle");
            } else {
                CHECK(e.key == "last" && e.tag == PackTag::Str, "entry 2 is the str");
            }
            ++seen;
        });
        CHECK(seen == 3, "for_each_entry visits every entry in insertion order");
    }
    CHECK(pool_live() == base, "walked pack balances the pool on drop");
}

// F1 regression: a pooled-class bin added while the ImagePool is EXHAUSTED must
// never resolve to a {nullptr, n>0} span.
static void test_bin_pool_exhaustion_no_null_span() {
    std::vector<xi_image_handle> hog;
    hog.reserve(70000);
    for (;;) {
        xi_image_handle h = xi::ImagePool::instance().create(1, 1, 1);
        if (!h) break;
        hog.push_back(h);
    }
    CHECK(!hog.empty(), "pool actually filled to exhaustion");
    CHECK(xi::ImagePool::instance().create(1, 1, 1) == XI_IMAGE_NULL,
          "pool is genuinely exhausted");

    const size_t N = 8192;
    std::vector<uint8_t> payload(N);
    for (size_t i = 0; i < N; ++i) payload[i] = uint8_t(i * 7 + 3);

    {
        PackBuilder b;
        b.add_bin("payload", payload.data(), N);
        Pack f = b.seal();
        auto v = f.get_bin("payload");
        bool poisoned = v.has_value() && v->data() == nullptr && v->size() > 0;
        CHECK(!poisoned, "Pack::get_bin never returns {nullptr, n>0} on pool exhaustion");
        if (v) {
            CHECK(v->data() != nullptr && v->size() == N,
                  "add_bin fell back to inline; data rode intact");
            bool ok = v->data() != nullptr && v->size() == N;
            for (size_t i = 0; ok && i < N; ++i) ok = ((*v)[i] == uint8_t(i * 7 + 3));
            CHECK(ok, "inline bin bytes intact under exhaustion");
        }
    }

    for (xi_image_handle h : hog) xi::ImagePool::instance().release(h);
}

// ------------------------------------------------------------------
// sort_idx recycle sanity (③): a stream of builds on one thread reusing the
// recycled scratch (incl. its sort_idx) must still produce correctly-sorted,
// correctly-ordered packs. Build many packs back-to-back with varied key counts
// and assert lookup + insertion order every time.
// ------------------------------------------------------------------
static void test_sort_idx_recycle() {
    bool all_ok = true;
    for (int round = 0; round < 200 && all_ok; ++round) {
        const int n = 1 + (round % 37);   // varied entry counts exercise resize()
        PackBuilder b;
        for (int i = 0; i < n; ++i)
            b.add_i64("key" + std::to_string((i * 7 + round) % n) + "_" + std::to_string(i),
                      int64_t(round) * 1000 + i);
        Pack f = b.seal();
        if (f.size() != size_t(n)) { all_ok = false; break; }
        // Insertion order preserved despite recycled sort_idx.
        int seen = 0; bool ordered = true;
        f.for_each([&](std::string_view key, PackTag) {
            std::string want = "key" + std::to_string((seen * 7 + round) % n) +
                               "_" + std::to_string(seen);
            if (key != want) ordered = false;
            ++seen;
        });
        if (!ordered || seen != n) { all_ok = false; break; }
        // Every key still resolves (hash-sorted directory rebuilt correctly).
        for (int i = 0; i < n; ++i) {
            std::string k = "key" + std::to_string((i * 7 + round) % n) +
                            "_" + std::to_string(i);
            auto v = f.get_i64(k);
            if (!v || *v != int64_t(round) * 1000 + i) { all_ok = false; break; }
        }
    }
    CHECK(all_ok, "recycled scratch/sort_idx builds correct packs across 200 rounds");
}

int main() {
    std::printf("test_xi_pack\n");
    test_lifecycle_and_contract_layer();
    test_bool_entry();
    test_duplicate_key();
    test_blob_roundtrip();
    test_add_blob_copy();
    test_image_as_convention();
    test_blob_head_validate_matrix();
    test_alloc_bytes_null_reject();
    test_canonical_walk_parity();
    test_for_each_entry_walk();
    test_offset_index_at_scale();
    test_seal_semantics();
    test_pooled_handle_balance();
    test_mixed_frame();
    test_crash_drop_no_double_release();
    test_bin_pool_exhaustion_no_null_span();
    test_sort_idx_recycle();
    if (g_fail == 0) { std::printf("  OK (all checks passed)\n"); return 0; }
    std::printf("  %d check(s) FAILED\n", g_fail);
    return 1;
}
