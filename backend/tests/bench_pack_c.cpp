//
// bench_pack_c.cpp — HEAD-TO-HEAD: current Pack (xi_pack.hpp) vs the
// "design C" prototype (xi/proto/xi_pack_c.hpp), same-shape workload on both.
// PROTOTYPE MEASUREMENT ONLY — not a perf gate, no baseline; the numbers feed
// the migrate/don't-migrate judgment.
//
// Harness: median of R batches of L ops after a warmup batch (median, not
// min, per the task spec — bench_pack's best_us min discipline is close but
// the median is the agreed statistic here). ns/op, plus a cur/C ratio column.
//
// LANES (each measured on both implementations):
//   1. build+seal+drop of the representative pack (2 tensors 1920x1200x1,
//      one 3.2 MB typed blob, 6 metadata entries incl. a msgpack tree) —
//      dominated by ~7.9 MB of pixel/blob memcpy on BOTH sides (equal work).
//   2. metadata read, 10 keys/iter (4 i64 + 4 f64 + 2 str — the spec's
//      2:2:1 mix, twice): current Pack::get_* (linear-scan container path),
//      current through the REAL ABI trampolines (xi_pack_v1 fn pointers ->
//      PackRegistry shared-lock resolve per call — the plugin-side cost),
//      and design C (one handle resolve + 10 directory binary-search reads).
//   3. tensor access: resolve+view 2 images — Pack::get_image (ImagePool
//      lookup) vs C as_tensor (BufTable lookup).
//   4. large-buffer alloc/free cycle 1920x1200x1 — ImagePool create/release
//      vs C size-class magazine. KNOWN ASYMMETRY (inherent to the designs,
//      documented, not equalized): ImagePool's create heap-allocates a
//      PoolEntry AND zero-fills 2.3 MB (vector::resize); C's magazine returns
//      cached, UNINITIALIZED memory. That difference IS design C's pitch.
//   5. serialize + deserialize of the representative pack. The current pack
//      has NO single-call serializer, so the current lane is the honest
//      record-plugin-equivalent manual walk (for_each + typed reads -> a flat
//      byte stream; rebuild through PackBuilder), vs C's slab-verbatim +
//      append-extern wire. Deserialize on both sides INCLUDES dropping the
//      rebuilt pack (kept symmetric).
//
// Honesty notes on equal work:
//   * Both builders write every key string per pack (C stages+copies keys
//     into the slab payload; current interns them into the arena).
//   * Both metadata lanes read through their PUBLIC lookup path (no cached
//     offsets on either side — bench_pack's precomputed-offset trick is a
//     schema-accessor optimization, out of scope for this comparison).
//   * C's blob rides its size-class pool; current's 3.2 MB bin rides
//     ImagePool via pack_pool (>= kPackLargeThreshold) — each design's own
//     large-path, as specified.
//
#include "xi/xi_pack.hpp"
#include "xi/xi_pack_abi.hpp"
#include "xi/xi_image_pool.hpp"
#include "xi/xi_mp.hpp"
#include "xi/proto/xi_pack_c.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using clk = std::chrono::steady_clock;
namespace pc = xi::packc;

static volatile uint64_t g_sink = 0;   // defeat dead-code elimination

// ---------------------------------------------------------------------------
// Harness: median of R batches of L ops (warmup batch first).
// ---------------------------------------------------------------------------
template <class F>
static double med_ns(F&& op, int L, int R) {
    for (int i = 0; i < L; ++i) op();          // warmup batch
    std::vector<double> xs;
    xs.reserve((size_t)R);
    for (int b = 0; b < R; ++b) {
        auto t0 = clk::now();
        for (int i = 0; i < L; ++i) op();
        auto t1 = clk::now();
        xs.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / L);
    }
    std::sort(xs.begin(), xs.end());
    return xs[xs.size() / 2];
}

static void row(const char* label, double cur_ns, double c_ns) {
    std::printf("  %-44s %14.1f %14.1f %10.2fx\n", label, cur_ns, c_ns,
                c_ns > 0 ? cur_ns / c_ns : 0.0);
}
static void row1(const char* label, double ns) {
    std::printf("  %-44s %14.1f %14s %10s\n", label, ns, "-", "-");
}

// ---------------------------------------------------------------------------
// Fixtures — the representative workload, identical bytes for both lanes.
// ---------------------------------------------------------------------------
static constexpr uint32_t kW = 1920, kH = 1200, kC = 1;
static constexpr size_t   kBlobBytes = 3200 * 1000;         // 3.2 MB
static constexpr uint16_t kBlobType  = pc::kTypeUserBase + 7;

struct Fixtures {
    std::vector<uint8_t> px0, px1, blob;
    xi::mp::Bytes region;
};
static Fixtures make_fixtures() {
    Fixtures f;
    f.px0.resize(size_t(kW) * kH * kC);
    f.px1.resize(f.px0.size());
    for (size_t i = 0; i < f.px0.size(); ++i) {
        f.px0[i] = uint8_t((i * 31 + 1) & 0xFF);
        f.px1[i] = uint8_t((i * 31 + 2) & 0xFF);
    }
    f.blob.resize(kBlobBytes);
    for (size_t i = 0; i < f.blob.size(); ++i) f.blob[i] = uint8_t((i * 7) & 0xFF);
    xi::mp::Writer w;
    w.map(4);
    w.key("area");  w.float_(142.5);
    w.key("cx");    w.float_(12.0);
    w.key("cy");    w.float_(34.0);
    w.key("label"); w.str("ok");
    f.region = w.take();
    return f;
}

// The representative pack, current implementation.
static xi::Pack build_current(const Fixtures& fx) {
    xi::PackBuilder b;
    b.add_str("$src", "cam0");
    b.add_i64("$seq", 424242);
    b.add_i64("ts_us", 1720000000123456);
    b.add_f64("score", 0.9375);
    b.add_f64("x", 100.5);
    b.add_mp("region", fx.region.data(), fx.region.size());
    b.add_image("img0", kW, kH, kC, fx.px0.data());
    b.add_image("img1", kW, kH, kC, fx.px1.data());
    b.add_bin("cal", fx.blob.data(), fx.blob.size());   // >= 4096 -> pooled
    return b.seal();
}
// The representative pack, design C.
static pc::PackHandleC build_c(const Fixtures& fx) {
    pc::PackBuilderC b;
    b.add_str("$src", "cam0");
    b.add_i64("$seq", 424242);
    b.add_i64("ts_us", 1720000000123456);
    b.add_f64("score", 0.9375);
    b.add_f64("x", 100.5);
    b.add_mp("region", fx.region.data(), fx.region.size());
    b.add_tensor("img0", kW, kH, kC, fx.px0.data());
    b.add_tensor("img1", kW, kH, kC, fx.px1.data());
    b.add_blob("cal", kBlobType, fx.blob.data(), fx.blob.size());
    return b.seal();
}

// ---------------------------------------------------------------------------
// 2. metadata packs — 10 keys: 4 i64 + 4 f64 + 2 str (the 2:2:1 mix, twice).
// ---------------------------------------------------------------------------
static const char* kMetaI64[4] = {"$seq", "ts_us", "count", "pass"};
static const char* kMetaF64[4] = {"score", "x", "y", "area"};
static const char* kMetaStr[2] = {"$src", "label"};

template <class AddI, class AddF, class AddS>
static void fill_meta(AddI&& ai, AddF&& af, AddS&& as) {
    ai(kMetaI64[0], 7);  ai(kMetaI64[1], 7000);
    ai(kMetaI64[2], 3);  ai(kMetaI64[3], 1);
    af(kMetaF64[0], 0.77);  af(kMetaF64[1], 100.5);
    af(kMetaF64[2], 50.25); af(kMetaF64[3], 142.5);
    as(kMetaStr[0], "matcher"); as(kMetaStr[1], "ok");
}

// ---------------------------------------------------------------------------
// 5. current-pack manual serializer — the record-plugin-equivalent walk.
// Flat stream: per entry {u8 tag, u32 key_len, key, u32 val_len, bytes} with
// images carrying {i32 w,h,c} before their pixels. This is what a
// record/replay plugin has to do today, since the current pack has no
// single-call serializer.
// ---------------------------------------------------------------------------
static void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    o.insert(o.end(), (const uint8_t*)&v, (const uint8_t*)&v + 4);
}
static void put_i32(std::vector<uint8_t>& o, int32_t v) {
    o.insert(o.end(), (const uint8_t*)&v, (const uint8_t*)&v + 4);
}
static std::vector<uint8_t> serialize_current(const xi::Pack& p) {
    std::vector<uint8_t> out;
    out.reserve(size_t(kW) * kH * kC * 2 + kBlobBytes + 4096);
    for (size_t i = 0; i < p.size(); ++i) {
        std::string_view key = p.key_at(i);
        xi::PackTag tag = p.tag_at(i);
        out.push_back(uint8_t(tag));
        put_u32(out, uint32_t(key.size()));
        out.insert(out.end(), key.begin(), key.end());
        if (tag == xi::PackTag::Image) {
            auto v = p.get_image(key);
            put_i32(out, v->width); put_i32(out, v->height); put_i32(out, v->channels);
            put_u32(out, uint32_t(v->pixels.size()));
            out.insert(out.end(), v->pixels.begin(), v->pixels.end());
        } else if (tag == xi::PackTag::Bin) {
            auto v = p.get_bin(key);
            put_u32(out, uint32_t(v->size()));
            out.insert(out.end(), v->begin(), v->end());
        } else {
            // Canonical bytes for the inline entry (I64/F64/Str/Mp/Bool).
            // POST-SLAB-MIGRATION: scalars live RAW in the pack, so the walk
            // re-emits the canonical form (Pack::canonical_value) instead of
            // splicing raw_at — this lane now measures the slab pack's
            // encode-on-serialize cost, the honest production walk.
            xi::mp::Writer w;
            bool ok = p.canonical_value(i, w);
            put_u32(out, ok ? uint32_t(w.size()) : 0u);
            if (ok) out.insert(out.end(), w.bytes().data(), w.bytes().data() + w.size());
        }
    }
    return out;
}
static xi::Pack deserialize_current(const std::vector<uint8_t>& in) {
    xi::PackBuilder b;
    size_t at = 0;
    auto get_u32 = [&] { uint32_t v; std::memcpy(&v, in.data() + at, 4); at += 4; return v; };
    auto get_i32 = [&] { int32_t v; std::memcpy(&v, in.data() + at, 4); at += 4; return v; };
    while (at < in.size()) {
        xi::PackTag tag = xi::PackTag(in[at]); ++at;
        uint32_t klen = get_u32();
        std::string_view key((const char*)in.data() + at, klen); at += klen;
        if (tag == xi::PackTag::Image) {
            int32_t w = get_i32(), h = get_i32(), c = get_i32();
            uint32_t n = get_u32();
            b.add_image(key, w, h, c, in.data() + at); at += n;
        } else if (tag == xi::PackTag::Bin) {
            uint32_t n = get_u32();
            b.add_bin(key, in.data() + at, n); at += n;
        } else {
            uint32_t n = get_u32();
            const uint8_t* v = in.data() + at; at += n;
            switch (tag) {
                case xi::PackTag::I64: b.add_i64(key, xi::pack_mp_detail::read_i64(v)); break;
                case xi::PackTag::F64: b.add_f64(key, xi::pack_mp_detail::read_f64(v)); break;
                case xi::PackTag::Bool: b.add_bool(key, xi::pack_mp_detail::read_bool(v)); break;
                case xi::PackTag::Str: b.add_str(key, xi::pack_mp_detail::read_str(v)); break;
                default:               b.add_mp(key, v, n); break;
            }
        }
    }
    return b.seal();
}

int main() {
    std::printf("bench_pack_c — current Pack (xi_pack.hpp) vs design-C prototype\n");
    std::printf("(median of R batches; ns/op; ratio = current / C, >1 means C is faster)\n\n");

    Fixtures fx = make_fixtures();

    std::printf("  %-44s %14s %14s %10s\n", "lane", "current ns", "design-C ns", "cur/C");
    std::printf("  %.100s\n",
        "----------------------------------------------------------------------------------------");

    // ---- 1. build + seal + drop, representative pack (~7.9 MB copied) ------
    {
        double cur = med_ns([&] { xi::Pack p = build_current(fx); g_sink += p.size(); },
                            8, 25);
        double c   = med_ns([&] {
                pc::PackHandleC h = build_c(fx);
                g_sink += h != pc::kPackNull;
                pc::release(h);
            }, 8, 25);
        row("build+seal+drop (repr. ~7.9MB pack)", cur, c);
    }

    // ---- 2. metadata read: 10 keys (4 i64 + 4 f64 + 2 str) -----------------
    {
        // current, direct container path
        xi::PackBuilder cb;
        fill_meta([&](const char* k, int64_t v) { cb.add_i64(k, v); },
                  [&](const char* k, double v)  { cb.add_f64(k, v); },
                  [&](const char* k, const char* v) { cb.add_str(k, v); });
        xi::Pack cp = cb.seal();
        double cur = med_ns([&] {
            uint64_t s = 0;
            for (const char* k : kMetaI64) s += (uint64_t)cp.get_i64(k).value_or(0);
            for (const char* k : kMetaF64) s += (uint64_t)(int64_t)cp.get_f64(k).value_or(0);
            for (const char* k : kMetaStr) s += cp.get_str(k).value_or(std::string_view{}).size();
            g_sink += s;
        }, 20000, 30);

        // current, through the REAL ABI trampolines (fn pointers -> registry
        // resolve per call) — the plugin-side cost of the same 10 reads.
        const xi_pack_v1* iface = xi::pack_v1_iface();
        xi_pack_builder ab = iface->builder_new();
        fill_meta([&](const char* k, int64_t v) { iface->builder_add_i64(ab, k, v); },
                  [&](const char* k, double v)  { iface->builder_add_f64(ab, k, v); },
                  [&](const char* k, const char* v) {
                      iface->builder_add_str(ab, k, v, (int32_t)std::strlen(v));
                  });
        xi_pack_handle ah = iface->builder_seal(ab);
        double abi = med_ns([&] {
            uint64_t s = 0;
            int64_t iv; double dv; const char* sp; int32_t sl;
            for (const char* k : kMetaI64) { if (iface->get_i64(ah, k, &iv)) s += (uint64_t)iv; }
            for (const char* k : kMetaF64) { if (iface->get_f64(ah, k, &dv)) s += (uint64_t)(int64_t)dv; }
            for (const char* k : kMetaStr) { if (iface->get_str(ah, k, &sp, &sl)) s += (uint64_t)sl; }
            g_sink += s;
        }, 20000, 30);

        // design C: one handle resolve + 10 directory reads per iteration.
        pc::PackBuilderC pb;
        fill_meta([&](const char* k, int64_t v) { pb.add_i64(k, v); },
                  [&](const char* k, double v)  { pb.add_f64(k, v); },
                  [&](const char* k, const char* v) { pb.add_str(k, v); });
        pc::PackHandleC ph = pb.seal();
        double c = med_ns([&] {
            pc::PackViewC pak(ph);           // handle resolve (per pack, real usage)
            uint64_t s = 0;
            for (const char* k : kMetaI64) s += (uint64_t)pak[k].as_i64();
            for (const char* k : kMetaF64) s += (uint64_t)(int64_t)pak[k].as_f64();
            for (const char* k : kMetaStr) s += pak[k].as_str().size();
            g_sink += s;
        }, 20000, 30);

        row("meta read 10 keys (current direct)", cur, c);
        row("meta read 10 keys (current ABI trampoline)", abi, c);
        iface->release(ah);
        pc::release(ph);
    }

    // ---- 3. tensor access: resolve + view 2 images --------------------------
    {
        xi::Pack cp = build_current(fx);
        pc::PackHandleC ph = build_c(fx);
        double cur = med_ns([&] {
            auto v0 = cp.get_image("img0");
            auto v1 = cp.get_image("img1");
            g_sink += (uint64_t)v0->width + v1->pixels[0] + v0->pixels[0] + (uint64_t)v1->height;
        }, 20000, 30);
        double c = med_ns([&] {
            pc::PackViewC pak(ph);
            pc::TensorViewC t0 = pak["img0"].as_tensor();
            pc::TensorViewC t1 = pak["img1"].as_tensor();
            g_sink += (uint64_t)t0.shape[0] + t1.data[0] + t0.data[0] + (uint64_t)t1.shape[1];
        }, 20000, 30);
        row("tensor resolve+view x2", cur, c);
        pc::release(ph);
    }

    // ---- 4. large-buffer alloc/free cycle, 1920x1200x1 ----------------------
    {
        auto& pool = xi::ImagePool::instance();
        double cur = med_ns([&] {
            xi_image_handle h = pool.create(kW, kH, kC);
            g_sink += (uint64_t)h;
            pool.release(h);
        }, 200, 30);
        const uint32_t shape[3] = {kW, kH, kC};
        double c = med_ns([&] {
            pc::BufHandleC h = pc::BufTable::instance().mint(
                pc::kTypeTensorU8, shape, uint64_t(kW) * kH * kC, nullptr);
            g_sink += (uint64_t)h;
            pc::BufTable::instance().release(h);
        }, 200, 30);
        row("big-buffer alloc/free 1920x1200 (see note)", cur, c);
    }

    // ---- 5. serialize + deserialize, representative pack --------------------
    {
        xi::Pack cp = build_current(fx);
        pc::PackHandleC ph = build_c(fx);

        double cur_ser = med_ns([&] {
            std::vector<uint8_t> w = serialize_current(cp);
            g_sink += w.size();
        }, 8, 25);
        double c_ser = med_ns([&] {
            std::vector<uint8_t> w = pc::serialize(ph);
            g_sink += w.size();
        }, 8, 25);
        row("serialize (repr. pack)", cur_ser, c_ser);

        std::vector<uint8_t> cur_wire = serialize_current(cp);
        std::vector<uint8_t> c_wire   = pc::serialize(ph);
        double cur_de = med_ns([&] {
            xi::Pack p = deserialize_current(cur_wire);   // + drop (symmetric)
            g_sink += p.size();
        }, 8, 25);
        double c_de = med_ns([&] {
            pc::PackHandleC h = pc::deserialize(c_wire.data(), c_wire.size());
            g_sink += h != pc::kPackNull;
            pc::release(h);                               // + drop (symmetric)
        }, 8, 25);
        row("deserialize+drop (repr. pack)", cur_de, c_de);
        row1("  (wire bytes: current)", (double)cur_wire.size());
        row1("  (wire bytes: design C)", (double)c_wire.size());
        pc::release(ph);
    }

    std::printf(
        "\nnotes:\n"
        "  * lane 4 asymmetry (inherent, not equalized): ImagePool create heap-allocates a\n"
        "    PoolEntry and ZERO-FILLS the 2.3MB pixel vector; the C magazine returns cached\n"
        "    uninitialized memory. Removing both costs is design C's stated point.\n"
        "  * lane 5 current = record-plugin-equivalent manual walk (no single-call\n"
        "    serializer exists for the current pack); C = slab memcpy + extern append.\n"
        "  * lane 2 ABI row uses the real xi_pack_v1 fn pointers (PackRegistry\n"
        "    shared-lock resolve per get_*) — the plugin-side cost today.\n");
    return 0;
}
