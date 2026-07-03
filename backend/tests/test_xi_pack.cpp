// test_xi_pack.cpp — unit tests for the v3 keyed-buffer Pack container
// (xi_pack.hpp). Covers the pack lifecycle (produce -> seal -> borrow ->
// drop), O(1) offset-index correctness at scale, immutability/seal semantics,
// pooled-handle balance verified against ImagePool's own stats, mixed
// small/large packs, and the drop-on-crash story (destruction == the release
// path, with no double-release across a move).
//
// It also exercises the _keys.h-style typed accessor layer from doc 02 against
// Pack, proving the contract layer carries over to the v3 representation
// unchanged.

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

// Live pool-handle count — the balance oracle. cumulative().live_now mirrors
// stats().handle_count via a cheap atomic; we use it to assert every pooled
// buffer a pack mints is released exactly once when the pack drops.
static int pool_live() { return xi::ImagePool::instance().cumulative().live_now; }

// ------------------------------------------------------------------
// The _keys.h-style contract layer (doc 02), applied to Pack. Key names are
// defined ONCE; a builder and an extractor compile from them. This is the same
// discipline v2 uses over the Record — proving it carries over to the v3 pack
// representation with only the underlying accessor calls changing.
// ------------------------------------------------------------------
namespace blob_keys {
inline constexpr std::string_view kThreshold = "threshold";
inline constexpr std::string_view kBlobCount = "blob_count";
inline constexpr std::string_view kMeanArea  = "mean_area";
inline constexpr std::string_view kLabel     = "label";
inline constexpr std::string_view kMask       = "mask";      // image entry
} // namespace blob_keys

struct BlobResult {
    int64_t threshold = 0;
    int64_t blob_count = 0;
    double  mean_area = 0.0;
    std::string label;
};

// builder: BlobResult -> pack entries (compiles down to Pack add_*).
static void build_blob(PackBuilder& b, const BlobResult& r) {
    b.add_i64(blob_keys::kThreshold, r.threshold);
    b.add_i64(blob_keys::kBlobCount, r.blob_count);
    b.add_f64(blob_keys::kMeanArea,  r.mean_area);
    b.add_str(blob_keys::kLabel,     r.label);
}
// extractor: pack -> BlobResult (fails loud on a missing required key).
static bool extract_blob(const Pack& f, BlobResult& out) {
    auto th = f.get_i64(blob_keys::kThreshold);
    auto bc = f.get_i64(blob_keys::kBlobCount);
    auto ma = f.get_f64(blob_keys::kMeanArea);
    auto lb = f.get_str(blob_keys::kLabel);
    if (!th || !bc || !ma || !lb) return false;   // required-key absence = fail-fast
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

    // doc-flavored get<i64>/get<f64> template aliases
    CHECK(f.get<int64_t>(blob_keys::kThreshold).value() == 128, "get<i64>");
    CHECK(f.get<double>(blob_keys::kMeanArea).value() == 42.5, "get<f64>");

    // Wrong-type read returns nullopt, never a garbage reinterpretation.
    CHECK(!f.get_str(blob_keys::kThreshold).has_value(), "type-mismatch read is nullopt");
    CHECK(!f.get_i64("missing").has_value(), "missing-key read is nullopt");
}

// Insertion-ordered walk + O(1) index correctness at scale.
static void test_offset_index_at_scale() {
    const int N = 2000;
    PackBuilder b;
    for (int i = 0; i < N; ++i)
        b.add_i64("k" + std::to_string(i), int64_t(i) * 3 + 1);
    Pack f = b.seal();
    CHECK(f.size() == size_t(N), "all N entries present");

    // Random-ish access: every key resolves to its exact value in O(1).
    bool all = true;
    for (int i = N - 1; i >= 0; --i) {
        auto v = f.get_i64("k" + std::to_string(i));
        if (!v || *v != int64_t(i) * 3 + 1) { all = false; break; }
    }
    CHECK(all, "every key resolves to its value after many inserts");

    // Insertion order preserved by the walk.
    int seen = 0; bool ordered = true;
    f.for_each([&](std::string_view key, PackTag) {
        if (key != std::string("k" + std::to_string(seen))) ordered = false;
        ++seen;
    });
    CHECK(seen == N && ordered, "for_each visits every entry in insertion order");
}

// Seal / immutability semantics observable in a release build (asserts are
// compiled out under NDEBUG, so we test the enforceable state, not the assert).
static void test_seal_semantics() {
    PackBuilder b;
    b.add_i64("x", 1);
    CHECK(!b.sealed(), "not sealed before seal()");
    Pack f = b.seal();
    CHECK(b.sealed(), "sealed flag set — further add_* would assert");
    // Pack exposes only const reads: there is no compile-time path to mutate a
    // sealed pack (enforced by the type — no non-const accessors exist).
    CHECK(f.get_i64("x").value() == 1, "sealed pack still reads");
}

// Pooled-handle balance: images/large bins mint pool buffers that must be
// released exactly once at pack drop. Verified against ImagePool's live count.
static void test_pooled_handle_balance() {
    const int base = pool_live();
    std::vector<uint8_t> px(64 * 48 * 3, 0xAB);
    std::vector<uint8_t> big(8192, 0xCD);   // >= threshold -> pooled bin
    {
        PackBuilder b;
        b.add_i64("n", 1);
        b.add_image("mask", 64, 48, 3, px.data());
        b.add_bin("payload", big.data(), big.size());
        Pack f = b.seal();
        CHECK(f.handle_count() == 2, "pack owns 2 pool handles (image + big bin)");
        CHECK(pool_live() == base + 2, "pool live count rose by 2 while pack alive");

        auto iv = f.get_image("mask");
        CHECK(iv && iv->width == 64 && iv->height == 48 && iv->channels == 3,
              "image descriptor round-trips");
        CHECK(iv && iv->pixels.size() == px.size() && iv->pixels[0] == 0xAB,
              "image pixels are a zero-copy view of the pool buffer");
        auto bin = f.get_bin("payload");
        CHECK(bin && bin->size() == big.size() && (*bin)[0] == 0xCD,
              "large bin resolves through the pool");
    }
    CHECK(pool_live() == base, "all pooled handles released when pack dropped");
}

// Mixed small/large pack: every storage class in one pack, all readable,
// handles balanced.
static void test_mixed_frame() {
    const int base = pool_live();
    std::vector<uint8_t> px(16 * 16, 7);
    uint8_t small_bin[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    {
        PackBuilder b;
        b.add_i64("i", -99);
        b.add_f64("f", 3.14159);
        b.add_str("s", "hello pack");
        b.add_bin("tiny", small_bin, sizeof small_bin);   // inline, no handle
        b.add_image("img", 16, 16, 1, px.data());         // pooled
        Pack f = b.seal();

        CHECK(f.handle_count() == 1, "only the image is pooled; tiny bin is inline");
        CHECK(pool_live() == base + 1, "one pool handle live");
        CHECK(f.get_i64("i").value() == -99, "negative i64 round-trips");
        CHECK(f.get_f64("f").value() == 3.14159, "f64 round-trips");
        CHECK(f.get_str("s").value() == "hello pack", "str round-trips");
        auto tb = f.get_bin("tiny");
        CHECK(tb && tb->size() == 8 && (*tb)[7] == 8, "inline tiny bin round-trips");
        CHECK(f.tag_of("tiny") == PackTag::Bin, "tiny bin tagged Bin regardless of storage");
        auto iv = f.get_image("img");
        CHECK(iv && iv->pixels.size() == 256 && iv->pixels[0] == 7, "image reads");
    }
    CHECK(pool_live() == base, "mixed pack balances the pool on drop");
}

// Drop-on-crash story: destruction IS the release path. A move transfers sole
// ownership; the moved-from pack releases nothing (no double-release), and
// exactly one release happens when the live owner dies.
static void test_crash_drop_no_double_release() {
    const int base = pool_live();
    std::vector<uint8_t> px(32 * 32 * 3, 0x11);
    {
        Pack moved_to;   // empty
        {
            PackBuilder b;
            b.add_image("mask", 32, 32, 3, px.data());
            Pack f = b.seal();
            CHECK(pool_live() == base + 1, "one handle live after seal");
            moved_to = std::move(f);
            // f is now moved-from: its scope exit must NOT release the handle.
        }
        CHECK(pool_live() == base + 1, "moved-from pack drop released nothing");
        CHECK(moved_to.handle_count() == 1, "ownership transferred to the move target");
    }
    CHECK(pool_live() == base, "the single live owner released exactly once");

    // A builder abandoned without seal() (a producer that faults mid-build)
    // still releases the handles it minted — no leak on the error path.
    {
        const int b2 = pool_live();
        {
            PackBuilder b;
            b.add_image("x", 8, 8, 3, nullptr);
            CHECK(pool_live() == b2 + 1, "unsealed builder minted a handle");
            // no seal() — builder destructs here
        }
        CHECK(pool_live() == b2, "unsealed builder released its handle on drop");
    }
}

// ==================================================================
// TypedPack<Schema> — the OFFSET-ACCESSOR read path (doc 07 §profile-1). The
// schema turns the contract key order into compile-time SLOTS; a declared field
// is read by direct slot index (no hash, no scan) and no key is ever interned.
// ==================================================================
namespace blob_schema {
struct Schema : xi::PackSchema<Schema> {
    static constexpr std::array<std::string_view, 6> keys = {
        "threshold", "blob_count", "mean_area", "label", "mask", "payload"};
    enum : int { kThreshold, kBlobCount, kMeanArea, kLabel, kMask, kPayload };
};
} // namespace blob_schema

using TSchema = blob_schema::Schema;
using TypedBlob = xi::TypedPack<TSchema>;
using TypedBlobBuilder = xi::TypedPackBuilder<TSchema>;

// COMPILE-TIME slot resolution: the key literal resolves to the same slot as the
// enumerator, entirely in constant evaluation (these are static_asserts — if the
// keyset drifted from the enum, this TU would fail to COMPILE, not at runtime).
static_assert(TSchema::slot_of("threshold") == TSchema::kThreshold, "slot(threshold)");
static_assert(TSchema::slot_of("mean_area") == TSchema::kMeanArea, "slot(mean_area)");
static_assert(TSchema::slot_of("label") == TSchema::kLabel, "slot(label)");
static_assert(TSchema::slot_of("not_declared") == -1, "undeclared key -> -1");
static_assert(TSchema::slot_count() == 6, "schema slot count");

static void test_typed_compile_time_slots() {
    TypedBlobBuilder b;
    b.set_i64<TSchema::kThreshold>(128);
    b.set_i64<TSchema::kBlobCount>(7);
    b.set_f64<TSchema::kMeanArea>(42.5);
    b.set_str<TSchema::kLabel>("pass");
    CHECK(!b.sealed(), "typed builder not sealed pre-seal");

    TypedBlob f = b.seal();
    CHECK(b.sealed(), "typed builder sealed after seal");
    CHECK(f.size() == 4, "typed pack has 4 set fields");
    CHECK(f.has<TSchema::kThreshold>(), "has<slot> for a set field");
    CHECK(!f.has<TSchema::kMask>(), "has<slot> false for a declared-but-unset field");
    CHECK(f.tag_of<TSchema::kMeanArea>() == xi::PackTag::F64, "tag_of<slot>");

    // Direct slot reads — the offset-accessor path (no hash, no scan).
    CHECK(f.get_i64<TSchema::kThreshold>().value() == 128, "get_i64<slot>");
    CHECK(f.get_i64<TSchema::kBlobCount>().value() == 7, "get_i64<slot>");
    CHECK(f.get_f64<TSchema::kMeanArea>().value() == 42.5, "get_f64<slot>");
    CHECK(f.get_str<TSchema::kLabel>().value() == "pass", "get_str<slot>");

    // Read by a key LITERAL resolved to a slot at compile time (get<i64>(kKey)).
    CHECK(f.get_i64<TSchema::slot_of("threshold")>().value() == 128, "read via slot_of(literal)");
    CHECK((f.get<int64_t, TSchema::kBlobCount>().value() == 7), "get<i64,slot>");
    CHECK((f.get<double, TSchema::kMeanArea>().value() == 42.5), "get<f64,slot>");

    // Wrong-type / unset reads are nullopt, never a garbage reinterpretation.
    CHECK(!f.get_str<TSchema::kThreshold>().has_value(), "type-mismatch slot read is nullopt");
    CHECK(!f.get_i64<TSchema::kMask>().has_value(), "unset slot read is nullopt");

    // Insertion/schema-order walk visits exactly the set declared fields.
    int seen = 0; bool ordered = true;
    const std::string_view expect[4] = {"threshold", "blob_count", "mean_area", "label"};
    f.for_each([&](std::string_view key, xi::PackTag) {
        if (seen >= 4 || key != expect[seen]) ordered = false;
        ++seen;
    });
    CHECK(seen == 4 && ordered, "for_each walks set declared fields in schema order");
}

// Mixed declared + dynamic keys in one typed pack: declared fields read by slot,
// undeclared keys through the string-keyed side list (the general fallback).
static void test_typed_mixed_declared_and_dynamic() {
    TypedBlobBuilder b;
    b.set_i64<TSchema::kThreshold>(200);
    b.set_str<TSchema::kLabel>("mixed");
    b.add_i64("extra_count", 99);          // undeclared -> dynamic side list
    b.add_str("note", "ad-hoc");
    TypedBlob f = b.seal();

    CHECK(f.size() == 4, "2 declared + 2 dynamic = 4 fields");
    CHECK(f.get_i64<TSchema::kThreshold>().value() == 200, "declared slot reads");
    CHECK(f.get_i64("extra_count").value() == 99, "dynamic key reads by string");
    CHECK(f.get_str("note").value() == "ad-hoc", "dynamic str reads by string");
    CHECK(f.has("extra_count") && f.has("threshold"), "has() spans declared + dynamic");
    CHECK(!f.has("absent"), "absent key not present");
    // A declared key is also reachable by its runtime string (the fallback path).
    CHECK(f.get_i64("threshold").value() == 200, "declared key reachable by string too");
    CHECK(!f.get_i64("note").has_value(), "wrong-type dynamic read is nullopt");

    int seen = 0;
    f.for_each([&](std::string_view, xi::PackTag) { ++seen; });
    CHECK(seen == 4, "for_each visits declared then dynamic");
}

// Arena recycling correctness: a stream of typed packs reuses the per-thread
// arena pool across builds. Each pack must read back its OWN values with no
// stale bleed from a prior (now-recycled) pack's chunk; and packs held alive
// simultaneously must not alias each other's recycled storage.
static void test_typed_arena_reuse_no_stale_bleed() {
    const int base = pool_live();

    // Sequential: build -> read -> drop, thousands of times. Each drop returns the
    // arena chunk to the pool; the next build reuses it and must overwrite cleanly.
    bool all = true;
    for (int i = 0; i < 5000; ++i) {
        TypedBlobBuilder b;
        b.set_i64<TSchema::kThreshold>(int64_t(i));
        b.set_f64<TSchema::kMeanArea>(double(i) * 1.5);
        b.set_str<TSchema::kLabel>(std::string("lbl") + std::to_string(i));
        TypedBlob f = b.seal();
        if (f.get_i64<TSchema::kThreshold>().value() != int64_t(i)) { all = false; break; }
        if (f.get_f64<TSchema::kMeanArea>().value() != double(i) * 1.5) { all = false; break; }
        if (f.get_str<TSchema::kLabel>().value() != std::string("lbl") + std::to_string(i)) { all = false; break; }
    }
    CHECK(all, "5000 recycled typed packs each read their own scalar+str values");

    // Simultaneously alive: two packs built back-to-back must hold independent
    // storage (the second cannot borrow the first's still-in-use chunk).
    {
        TypedBlobBuilder ba;
        ba.set_i64<TSchema::kThreshold>(11);
        ba.set_str<TSchema::kLabel>("first");
        TypedBlob fa = ba.seal();

        TypedBlobBuilder bb;
        bb.set_i64<TSchema::kThreshold>(22);
        bb.set_str<TSchema::kLabel>("second");
        TypedBlob fb = bb.seal();

        CHECK(fa.get_i64<TSchema::kThreshold>().value() == 11 &&
              fa.get_str<TSchema::kLabel>().value() == "first",
              "first live pack keeps its values while a second is built");
        CHECK(fb.get_i64<TSchema::kThreshold>().value() == 22 &&
              fb.get_str<TSchema::kLabel>().value() == "second",
              "second live pack holds independent values");
    }

    CHECK(pool_live() == base, "no pool handles leaked across the reuse stream");
}

// Typed pooled-handle balance: image + large bin in declared slots mint pool
// buffers released exactly once at drop; a move transfers sole ownership.
static void test_typed_pooled_handle_balance() {
    const int base = pool_live();
    std::vector<uint8_t> px(64 * 48 * 3, 0xAB);
    std::vector<uint8_t> big(8192, 0xCD);   // >= threshold -> pooled bin
    {
        TypedBlobBuilder b;
        b.set_i64<TSchema::kThreshold>(1);
        b.set_image<TSchema::kMask>(64, 48, 3, px.data());
        b.set_bin<TSchema::kPayload>(big.data(), big.size());
        TypedBlob f = b.seal();
        CHECK(f.handle_count() == 2, "typed pack owns 2 pool handles (image + big bin)");
        CHECK(pool_live() == base + 2, "pool live rose by 2 while typed pack alive");

        auto iv = f.get_image<TSchema::kMask>();
        CHECK(iv && iv->width == 64 && iv->height == 48 && iv->channels == 3,
              "typed image descriptor round-trips");
        CHECK(iv && iv->pixels.size() == px.size() && iv->pixels[0] == 0xAB,
              "typed image pixels are a zero-copy view");
        auto bin = f.get_bin<TSchema::kPayload>();
        CHECK(bin && bin->size() == big.size() && (*bin)[0] == 0xCD,
              "typed large bin resolves through the pool");

        // Move transfers ownership; the moved-from pack releases nothing.
        TypedBlob moved = std::move(f);
        CHECK(pool_live() == base + 2, "move did not release");
        CHECK(moved.handle_count() == 2, "handles transferred to the move target");
    }
    CHECK(pool_live() == base, "typed pack released both handles once on drop");
}

int main() {
    std::printf("test_xi_pack\n");
    test_lifecycle_and_contract_layer();
    test_offset_index_at_scale();
    test_seal_semantics();
    test_pooled_handle_balance();
    test_mixed_frame();
    test_crash_drop_no_double_release();
    test_typed_compile_time_slots();
    test_typed_mixed_declared_and_dynamic();
    test_typed_arena_reuse_no_stale_bleed();
    test_typed_pooled_handle_balance();
    if (g_fail == 0) { std::printf("  OK (all checks passed)\n"); return 0; }
    std::printf("  %d check(s) FAILED\n", g_fail);
    return 1;
}
