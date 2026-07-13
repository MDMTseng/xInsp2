// test_pack_c.cpp — correctness net for the "Pack design C" PROTOTYPE
// (backend/include/xi/proto/xi_pack_c.hpp). Prototype-scoped: this guards the
// experiment's claims so its bench numbers mean something, not a production
// contract.
//
// Covered, per the agreed spec:
//   * representative pack: 2 tensors 1920x1200x1 + one 3.2 MB custom blob
//     (magic/version header, user type_id) + 6 metadata entries (incl. a
//     msgpack tree via xi::mp::Writer and $src/$seq style keys);
//   * every entry read back TYPED through the EntryView API;
//   * type-mismatch fail-loud (SoftFailScope + mismatch counter oracle);
//   * retain/release + cross-pack adopt (shared tensor rc accounting, both
//     packs read the SAME bytes);
//   * serialize -> deserialize round-trip, byte-equality on every entry;
//   * destroy-order stress: producer pack dropped FIRST, adopting pack still
//     reads the shared buffer (rc keeps it alive);
//   * table-level leak oracles (BufTable/PackTableC live counts) at the end.

#include "xi/proto/xi_pack_c.hpp"
#include "xi/xi_mp.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

namespace pc = xi::packc;

// The custom blob's declared type (user space) + its embedded header.
static constexpr uint16_t kBlobTypeCal = pc::kTypeUserBase + 7;
static constexpr uint32_t kBlobMagic   = 0x31424C42u;   // 'BLB1'
static constexpr uint32_t kBlobVersion = 3;
static constexpr size_t   kBlobBytes   = 3200 * 1000;   // the 3.2 MB blob
static constexpr uint32_t kW = 1920, kH = 1200, kC = 1;

// ------------------------------------------------------------------
// Deterministic fixture payloads (patterned so byte-equality checks bite).
// ------------------------------------------------------------------
static std::vector<uint8_t> make_pixels(uint8_t seed) {
    std::vector<uint8_t> px(size_t(kW) * kH * kC);
    for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t((i * 31 + seed) & 0xFF);
    return px;
}
static std::vector<uint8_t> make_blob() {
    std::vector<uint8_t> b(kBlobBytes);
    std::memcpy(b.data(), &kBlobMagic, 4);
    std::memcpy(b.data() + 4, &kBlobVersion, 4);
    for (size_t i = 8; i < b.size(); ++i) b[i] = uint8_t((i * 7 + 13) & 0xFF);
    return b;
}
static xi::mp::Bytes make_region_mp() {
    xi::mp::Writer w;
    w.map(4);
    w.key("area");  w.float_(142.5);
    w.key("cx");    w.float_(12.0);
    w.key("cy");    w.float_(34.0);
    w.key("label"); w.str("ok");
    return w.take();
}

// The representative pack of the spec — shared by several tests below.
static pc::PackHandleC build_representative(const std::vector<uint8_t>& px0,
                                            const std::vector<uint8_t>& px1,
                                            const std::vector<uint8_t>& blob,
                                            const xi::mp::Bytes& region) {
    pc::PackBuilderC b;
    b.add_str("$src", "cam0");
    b.add_i64("$seq", 424242);
    b.add_i64("ts_us", 1720000000123456);
    b.add_f64("score", 0.9375);
    b.add_f64("x", 100.5);
    b.add_mp("region", region.data(), region.size());
    CHECK(b.add_tensor("img0", kW, kH, kC, px0.data()), "add_tensor img0");
    CHECK(b.add_tensor("img1", kW, kH, kC, px1.data()), "add_tensor img1");
    CHECK(b.add_blob("cal", kBlobTypeCal, blob.data(), blob.size()), "add_blob cal");
    return b.seal(/*ts_us=*/777);
}

// Read every entry back typed and check it against the fixtures. Used on the
// original AND the deserialized pack (byte-equality both times).
static void check_all_entries(pc::PackHandleC h,
                              const std::vector<uint8_t>& px0,
                              const std::vector<uint8_t>& px1,
                              const std::vector<uint8_t>& blob,
                              const xi::mp::Bytes& region,
                              const char* label) {
    pc::PackViewC pak(h);
    CHECK(pak.valid(), label);
    CHECK(pak.size() == 9, "entry count == 9");
    CHECK(pak.header()->magic == pc::kMagic, "header magic");
    CHECK(pak.header()->ts_us == 777, "header ts_us");

    CHECK(pak["$src"].as_str() == "cam0", "$src");
    CHECK(pak["$seq"].as_i64() == 424242, "$seq");
    CHECK(pak["ts_us"].as_i64() == 1720000000123456, "ts_us");
    CHECK(pak["score"].as_f64() == 0.9375, "score");
    CHECK(pak["x"].as_f64() == 100.5, "x");

    auto mp = pak["region"].as_mp();
    CHECK(mp.size() == region.size() &&
          std::memcmp(mp.data(), region.data(), region.size()) == 0,
          "region msgpack bytes");
    // The mp bytes are real canonical msgpack: a stock Reader must parse them.
    {
        xi::mp::Reader r(mp.data(), mp.size());
        xi::mp::Element top;
        CHECK(r.next(top) == xi::mp::Status::Ok && top.kind == xi::mp::Kind::Map &&
              top.len == 4, "region parses as a 4-entry map");
    }

    for (int t = 0; t < 2; ++t) {
        const auto& px = t == 0 ? px0 : px1;
        pc::TensorViewC tv = pak[t == 0 ? "img0" : "img1"].as_tensor();
        CHECK(tv.ok(), "tensor resolves");
        CHECK(tv.shape[0] == kW && tv.shape[1] == kH && tv.shape[2] == kC,
              "tensor shape");
        CHECK(tv.bytes == px.size(), "tensor byte size");
        CHECK((reinterpret_cast<uintptr_t>(tv.data) & 63) == 0,
              "tensor 64B-aligned");
        CHECK(tv.data && std::memcmp(tv.data, px.data(), px.size()) == 0,
              "tensor bytes equal");
    }

    auto bl = pak["cal"].as_blob(kBlobTypeCal);
    CHECK(bl.size() == blob.size() &&
          std::memcmp(bl.data(), blob.data(), blob.size()) == 0,
          "blob bytes equal");
    uint32_t magic = 0, ver = 0;
    if (bl.size() >= 8) {
        std::memcpy(&magic, bl.data(), 4);
        std::memcpy(&ver, bl.data() + 4, 4);
    }
    CHECK(magic == kBlobMagic && ver == kBlobVersion, "blob magic/version header");

    CHECK(!pak["nope"].valid(), "missing key is invalid (quiet)");
}

// ------------------------------------------------------------------
static void test_representative_pack() {
    std::printf("representative pack build + typed reads...\n");
    auto px0 = make_pixels(1), px1 = make_pixels(2);
    auto blob = make_blob();
    auto region = make_region_mp();

    pc::PackHandleC h = build_representative(px0, px1, blob, region);
    CHECK(h != pc::kPackNull, "seal");
    check_all_entries(h, px0, px1, blob, region, "original pack view");

    // Directory iteration covers all 9 entries and finds every key once.
    pc::PackViewC pak(h);
    int seen = 0;
    for (uint32_t i = 0; i < pak.size(); ++i)
        if (pak.at(i).valid()) ++seen;
    CHECK(seen == 9, "iteration sees all 9 entries");

    pc::release(h);
}

static void test_type_mismatch_fail_loud() {
    std::printf("type-mismatch fail-loud...\n");
    pc::PackBuilderC b;
    b.add_i64("n", 5);
    b.add_str("s", "abc");
    auto px = make_pixels(3);
    b.add_tensor("t", kW, kH, kC, px.data());
    pc::PackHandleC h = b.seal();
    pc::PackViewC pak(h);

    uint64_t before = pc::type_mismatch_count().load();
    {
        // SoftFailScope: suppress the Debug assert so this test runs the
        // Release behaviour (warn + null value + counter bump) in BOTH configs.
        pc::SoftFailScope soft;
        CHECK(pak["n"].as_f64() == 0.0, "i64 read as f64 -> 0.0");
        CHECK(pak["n"].as_str().empty(), "i64 read as str -> empty");
        CHECK(!pak["s"].as_tensor().ok(), "str read as tensor -> not ok");
        CHECK(pak["t"].as_i64() == 0, "tensor read as i64 -> 0");
        CHECK(pak["t"].as_blob(kBlobTypeCal).empty(),
              "tensor read as wrong blob type -> empty");
    }
    uint64_t after = pc::type_mismatch_count().load();
    CHECK(after - before == 5, "each mismatch counted exactly once");

    // Missing key stays QUIET (absence is not a type bug).
    before = pc::type_mismatch_count().load();
    CHECK(pak["absent"].as_i64() == 0, "missing key -> 0, no assert");
    CHECK(pc::type_mismatch_count().load() == before, "missing key not counted");

    pc::release(h);
}

static void test_retain_release_and_adopt() {
    std::printf("retain/release + cross-pack adopt...\n");
    auto& bt = pc::BufTable::instance();
    auto& pt = pc::PackTableC::instance();

    auto px = make_pixels(4);
    pc::PackBuilderC b1;
    b1.add_i64("$seq", 1);
    b1.add_tensor("shared", kW, kH, kC, px.data());
    pc::PackHandleC p1 = b1.seal();

    // Pack rc mechanics: retain bumps, release drops, slab survives until 0.
    CHECK(pt.refcount(p1) == 1, "pack rc starts at 1");
    pc::retain(p1);
    CHECK(pt.refcount(p1) == 2, "retain -> rc 2");
    pc::release(p1);
    CHECK(pt.refcount(p1) == 1, "release -> rc 1");

    pc::BufHandleC shared = pc::PackViewC(p1)["shared"].buf_handle();
    CHECK(shared != pc::kBufNull, "shared tensor handle");
    CHECK(bt.refcount(shared) == 1, "buffer rc 1 (p1 sole owner)");

    // Adopt into a second pack: rc 2, both packs read the SAME bytes (and in
    // fact the same pointer — zero-copy share).
    pc::PackBuilderC b2;
    b2.add_i64("$seq", 2);
    CHECK(b2.adopt("borrowed", shared), "adopt");
    CHECK(bt.refcount(shared) == 2, "adopt -> buffer rc 2");
    pc::PackHandleC p2 = b2.seal();

    pc::TensorViewC t1 = pc::PackViewC(p1)["shared"].as_tensor();
    pc::TensorViewC t2 = pc::PackViewC(p2)["borrowed"].as_tensor();
    CHECK(t1.ok() && t2.ok(), "both packs resolve the tensor");
    CHECK(t1.data == t2.data, "zero-copy: same buffer pointer");
    CHECK(t2.bytes == px.size() &&
          std::memcmp(t2.data, px.data(), px.size()) == 0,
          "adopted read sees the producer's bytes");

    // Adopting a dead handle must refuse (stale-handle discipline).
    {
        pc::PackBuilderC b3;
        CHECK(!b3.adopt("dead", pc::kBufNull), "adopt(null) refused");
    }

    pc::release(p1);
    CHECK(bt.refcount(shared) == 1, "p1 drop released its buffer ref");
    pc::release(p2);
    CHECK(bt.refcount(shared) == 0, "p2 drop freed the buffer (stale rc reads 0)");
    CHECK(bt.data(shared) == nullptr, "stale handle resolves null after free");
}

static void test_destroy_order_stress() {
    std::printf("destroy-order stress (producer drops first)...\n");
    auto px = make_pixels(5);

    pc::PackBuilderC prod;
    prod.add_str("$src", "producer");
    prod.add_tensor("frame", kW, kH, kC, px.data());
    pc::PackHandleC producer = prod.seal();

    pc::BufHandleC fh = pc::PackViewC(producer)["frame"].buf_handle();
    pc::PackBuilderC rdr;
    rdr.add_str("$src", "reader");
    CHECK(rdr.adopt("frame", fh), "reader adopts");
    pc::PackHandleC reader = rdr.seal();

    // Producer dies FIRST. The reader pack must remain fully valid: handle
    // resolve, shape, and every byte.
    pc::release(producer);
    CHECK(pc::PackTableC::instance().slab(producer) == nullptr,
          "producer handle is dead");
    pc::TensorViewC tv = pc::PackViewC(reader)["frame"].as_tensor();
    CHECK(tv.ok(), "reader still resolves after producer death");
    CHECK(tv.shape[0] == kW && tv.shape[1] == kH, "shape survives");
    CHECK(std::memcmp(tv.data, px.data(), px.size()) == 0, "bytes survive");
    pc::release(reader);

    // Churn a few generations through the same slots: stale handles must keep
    // failing resolve (generation discipline), fresh ones must work.
    pc::PackHandleC stale = reader;
    for (int i = 0; i < 32; ++i) {
        pc::PackBuilderC b;
        b.add_i64("i", i);
        pc::PackHandleC h = b.seal();
        CHECK(pc::PackViewC(h)["i"].as_i64() == i, "fresh pack reads");
        CHECK(pc::PackTableC::instance().slab(stale) == nullptr,
              "stale pack handle never resurrects");
        pc::release(h);
    }
}

static void test_raii_refs() {
    std::printf("RAII refs (PackRef/BufRef)...\n");
    auto& bt = pc::BufTable::instance();
    auto& pt = pc::PackTableC::instance();
    const int32_t buf_before = bt.live(), pack_before = pt.live();

    auto px = make_pixels(8);
    {
        pc::PackBuilderC b;
        b.add_i64("$seq", 9);
        b.add_tensor("img", kW, kH, kC, px.data());
        pc::PackRef pk = b.seal_ref();               // adopts the seal's rc=1
        CHECK(bool(pk), "seal_ref yields a live ref");
        CHECK(pt.refcount(pk.get()) == 1, "seal_ref rc starts at 1 (adopting)");

        // copy = retain (liveness: rc visibly bumps, and the copy keeps the
        // pack alive after the original is reset).
        pc::PackRef copy = pk;
        CHECK(pt.refcount(pk.get()) == 2, "PackRef copy retained (rc 2)");
        pc::PackHandleC raw = pk.get();
        pk.reset();
        CHECK(pt.slab(raw) != nullptr, "copy keeps the pack alive after reset");
        CHECK(pt.refcount(raw) == 1, "reset dropped exactly one ref");

        // move = steal (no rc change).
        pc::PackRef moved = std::move(copy);
        CHECK(pt.refcount(raw) == 1, "move steals, rc unchanged");
        CHECK(!copy, "moved-from ref is empty");

        // View from a PackRef + owning BufRef out of an entry.
        pc::PackViewC pak(moved);
        CHECK(pak["$seq"].as_i64() == 9, "PackViewC(const PackRef&) reads");
        pc::BufHandleC braw = pak["img"].buf_handle();
        CHECK(bt.refcount(braw) == 1, "buffer rc 1 (pack sole owner)");
        pc::BufRef bref = pak["img"].buf_ref();      // addref'd co-ref
        CHECK(bt.refcount(braw) == 2, "buf_ref took its own ref (rc 2)");
        {
            pc::BufRef bcopy = bref;                 // copy = addref
            CHECK(bt.refcount(braw) == 3, "BufRef copy addref'd (rc 3)");
        }                                            // scope-exit release
        CHECK(bt.refcount(braw) == 2, "BufRef copy released at scope exit");

        // adopt(key, const BufRef&): the pack takes its OWN ref, the
        // caller's BufRef stays untouched and owning.
        pc::PackBuilderC b2;
        CHECK(b2.adopt("borrowed", bref), "adopt(BufRef)");
        CHECK(bt.refcount(braw) == 3, "adopt took its own ref");
        CHECK(bref.get() == braw, "caller's BufRef untouched");
        pc::PackRef p2 = b2.seal_ref();

        // BufRef outlives every pack: drop both packs, buffer must survive.
        moved.reset();
        p2.reset();
        CHECK(bt.data(braw) != nullptr, "BufRef alone keeps the buffer alive");
        CHECK(bt.refcount(braw) == 1, "packs gone, BufRef holds the last ref");
        CHECK(std::memcmp(bt.data(braw), px.data(), px.size()) == 0,
              "bytes intact under BufRef-only ownership");
    } // <- bref/pk/copy/moved/p2 all destroyed here
    // Scope-exit release proof: the table leak oracles are back to baseline.
    CHECK(bt.live() == buf_before, "scope exit released every buffer (oracle)");
    CHECK(pt.live() == pack_before, "scope exit released every pack (oracle)");
}

static void test_tensor_dtype() {
    std::printf("tensor dtype (f32 build/read/fail-loud/round-trip)...\n");
    const uint32_t W = 320, H = 40, C = 2;
    std::vector<float> zf(size_t(W) * H * C);
    for (size_t i = 0; i < zf.size(); ++i) zf[i] = float(i) * 0.5f - 100.0f;

    pc::PackBuilderC b;
    b.add_i64("$seq", 31);
    CHECK(b.add_tensor("z", W, H, C, pc::kDtypeF32, zf.data()), "add f32 tensor");
    pc::PackRef pk = b.seal_ref();
    pc::PackViewC pak(pk);

    // Untyped view carries the dtype + elem size; bytes = w*h*c*4.
    pc::TensorViewC tv = pak["z"].as_tensor();
    CHECK(tv.ok(), "as_tensor resolves the f32 tensor");
    CHECK(tv.type_id == pc::kTypeTensorF32, "type_id is kTypeTensorF32");
    CHECK(tv.dtype == pc::kDtypeF32 && tv.elem_size == 4, "dtype/elem_size");
    CHECK(tv.bytes == zf.size() * sizeof(float), "bytes = w*h*c*elem_size");

    // Element-typed read: right dtype resolves, count in elements.
    pc::TensorOfC<float> tf = pak["z"].as_tensor_of<float>();
    CHECK(tf.ok(), "as_tensor_of<float> resolves");
    CHECK(tf.count == zf.size(), "element count");
    CHECK(tf.shape[0] == W && tf.shape[1] == H && tf.shape[2] == C, "shape");
    CHECK(std::memcmp(tf.data, zf.data(), zf.size() * sizeof(float)) == 0,
          "f32 bytes equal");

    // Wrong dtype is fail-loud (counter oracle), same as as_blob's discipline.
    uint64_t before = pc::type_mismatch_count().load();
    {
        pc::SoftFailScope soft;
        CHECK(!pak["z"].as_tensor_of<double>().ok(), "f32 read as f64 refused");
        CHECK(!pak["z"].as_tensor_of<uint8_t>().ok(), "f32 read as u8 refused");
        CHECK(!pak["$seq"].as_tensor_of<float>().ok(), "i64 read as tensor refused");
    }
    CHECK(pc::type_mismatch_count().load() - before == 3,
          "each dtype mismatch counted once");

    // Round-trip: dtype travels in the type_id, byte-identical payload.
    std::vector<uint8_t> wire = pc::serialize(pk.get());
    pc::PackRef pk2(pc::deserialize(wire.data(), wire.size()));
    CHECK(bool(pk2), "dtype pack deserializes");
    pc::TensorOfC<float> tf2 = pc::PackViewC(pk2)["z"].as_tensor_of<float>();
    CHECK(tf2.ok(), "round-tripped tensor still reads as f32");
    CHECK(pc::PackViewC(pk2)["z"].as_tensor().dtype == pc::kDtypeF32,
          "round-tripped dtype");
    CHECK(tf2.count == tf.count &&
          std::memcmp(tf2.data, tf.data, tf.count * sizeof(float)) == 0,
          "round-trip f32 bytes IDENTICAL");
}

static void test_serialize_roundtrip() {
    std::printf("serialize -> deserialize round-trip...\n");
    auto px0 = make_pixels(6), px1 = make_pixels(7);
    auto blob = make_blob();
    auto region = make_region_mp();

    pc::PackHandleC h = build_representative(px0, px1, blob, region);
    std::vector<uint8_t> wire = pc::serialize(h);
    CHECK(!wire.empty(), "serialize produced bytes");
    // Expected size: slab + 2 tensors + blob, each with a 22-byte record head.
    pc::PackViewC pak(h);
    uint64_t expect = pak.header()->slab_bytes +
                      3 * 22 + 2 * uint64_t(kW) * kH * kC + kBlobBytes;
    CHECK(wire.size() == expect, "wire size exactly as specified");

    pc::PackHandleC h2 = pc::deserialize(wire.data(), wire.size());
    CHECK(h2 != pc::kPackNull, "deserialize");
    CHECK(h2 != h, "round-trip is a NEW pack");
    check_all_entries(h2, px0, px1, blob, region, "deserialized pack view");

    // The re-minted buffers are INDEPENDENT copies (patched handles).
    pc::BufHandleC b1 = pc::PackViewC(h)["img0"].buf_handle();
    pc::BufHandleC b2 = pc::PackViewC(h2)["img0"].buf_handle();
    CHECK(b1 != b2, "deserialized tensor has its own buffer");

    // Truncated input must fail cleanly and leak nothing.
    auto& bt = pc::BufTable::instance();
    int32_t live_before = bt.live();
    CHECK(pc::deserialize(wire.data(), wire.size() - 1) == pc::kPackNull,
          "truncated wire rejected");
    CHECK(pc::deserialize(wire.data(), sizeof(pc::PackHeaderC) - 1) == pc::kPackNull,
          "header-short wire rejected");
    std::vector<uint8_t> bad(wire.begin(), wire.begin() + 256);
    bad[0] ^= 0xFF;   // break the magic
    CHECK(pc::deserialize(bad.data(), bad.size()) == pc::kPackNull,
          "bad magic rejected");
    CHECK(bt.live() == live_before, "failed deserialize leaked no buffers");

    pc::release(h);
    pc::release(h2);
}

int main() {
    std::printf("=== test_pack_c (design-C prototype) ===\n");
    auto& bt = pc::BufTable::instance();
    auto& pt = pc::PackTableC::instance();
    const int32_t buf0 = bt.live(), pack0 = pt.live();

    test_representative_pack();
    test_type_mismatch_fail_loud();
    test_retain_release_and_adopt();
    test_destroy_order_stress();
    test_raii_refs();
    test_tensor_dtype();
    test_serialize_roundtrip();

    // Leak oracle: every buffer and pack minted above was released.
    CHECK(bt.live() == buf0, "no buffer leaked");
    CHECK(pt.live() == pack0, "no pack leaked");

    if (g_fail) { std::printf("FAILED: %d check(s)\n", g_fail); return 1; }
    std::printf("all pack_c tests passed\n");
    return 0;
}
