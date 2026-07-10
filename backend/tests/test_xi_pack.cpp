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

// ------------------------------------------------------------------
// Bool entry (pack-plane hardening — the json_source bool-entry gap): builder ->
// canonical 0xc2/0xc3 arena byte -> tagged walk -> typed reads, on BOTH access
// paths, fail-closed against i64 in both directions (an i64 0/1 is NOT a bool).
// ------------------------------------------------------------------
static void test_bool_entry() {
    // Dynamic (string-keyed) path.
    PackBuilder b;
    b.add_bool("pass", true);
    b.add_bool("fail", false);
    b.add_i64("one", 1);
    Pack f = b.seal();
    CHECK(f.tag_of("pass") == PackTag::Bool, "bool entry stores the Bool tag");
    CHECK(f.get_bool("pass").value() == true,  "get_bool true round-trips");
    CHECK(f.get_bool("fail").value() == false, "get_bool false round-trips");
    CHECK(f.get<bool>("pass").value() == true, "get<bool> alias");
    // Fail-closed BOTH directions: no silent bool<->i64 coercion.
    CHECK(!f.get_i64("pass").has_value(), "bool entry refuses an i64 read");
    CHECK(!f.get_bool("one").has_value(), "i64 entry refuses a bool read");
    // The stored small-plane bytes ARE the canonical msgpack bool — the single
    // 0xc2/0xc3 byte a generic dumper splices to the wire verbatim (memory==wire).
    CHECK(f.raw_at(0).size() == 1 && f.raw_at(0)[0] == 0xc3, "canonical true byte 0xc3");
    CHECK(f.raw_at(1).size() == 1 && f.raw_at(1)[0] == 0xc2, "canonical false byte 0xc2");
    int bools = 0;
    f.for_each([&](std::string_view, PackTag t) { if (t == PackTag::Bool) ++bools; });
    CHECK(bools == 2, "generic walk reports the Bool tag");
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

// F1 regression: a pooled-class bin (>= kPackLargeThreshold) added while the
// ImagePool is EXHAUSTED must never resolve to a {nullptr, n>0} span. Pre-fix,
// add_bin/set_bin stored {pooled=true, handle=0, inl_len=n} on a failed pool
// alloc and get_bin did view(0).first(n) -> span{nullptr, n} (UB / OOB read).
// The fix falls back to INLINE arena storage (producer honesty) and guards
// get_bin's pooled branch (consumer safety). Covers the dynamic Pack path.
static void test_bin_pool_exhaustion_no_null_span() {
    // Drain the pool with minimal 1x1x1 handles so the next alloc_bytes fails.
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

    const size_t N = 8192;   // >= kPackLargeThreshold (4096) -> pooled class
    std::vector<uint8_t> payload(N);
    for (size_t i = 0; i < N; ++i) payload[i] = uint8_t(i * 7 + 3);

    // --- Dynamic Pack::add_bin under exhaustion ---
    {
        PackBuilder b;
        b.add_bin("payload", payload.data(), N);
        Pack f = b.seal();
        auto v = f.get_bin("payload");
        // Never a poisoned present-but-null span.
        bool poisoned = v.has_value() && v->data() == nullptr && v->size() > 0;
        CHECK(!poisoned, "Pack::get_bin never returns {nullptr, n>0} on pool exhaustion");
        if (v) {
            CHECK(v->data() != nullptr && v->size() == N,
                  "add_bin fell back to inline; data rode intact (dynamic)");
            bool ok = v->data() != nullptr && v->size() == N;
            for (size_t i = 0; ok && i < N; ++i) ok = ((*v)[i] == uint8_t(i * 7 + 3));
            CHECK(ok, "dynamic bin bytes intact under exhaustion");
        }
    }

    // Restore the pool for subsequent tests.
    for (xi_image_handle h : hog) xi::ImagePool::instance().release(h);
}

int main() {
    std::printf("test_xi_pack\n");
    test_lifecycle_and_contract_layer();
    test_bool_entry();
    test_offset_index_at_scale();
    test_seal_semantics();
    test_pooled_handle_balance();
    test_mixed_frame();
    test_crash_drop_no_double_release();
    test_bin_pool_exhaustion_no_null_span();
    if (g_fail == 0) { std::printf("  OK (all checks passed)\n"); return 0; }
    std::printf("  %d check(s) FAILED\n", g_fail);
    return 1;
}
