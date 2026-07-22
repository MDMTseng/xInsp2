// blob_analysis example — min_area is the noise gate, and `blobs` is geometry.
//
// The one thing to learn here: blob_analysis does not just count bright things.
// It returns, per blob, the AREA, the CENTROID and the BOUNDING BOX (plus the
// traced contour) as one nested msgpack entry — and `min_area` is the knob that
// decides which of those blobs are parts and which are dirt on the lens.
//
// So the example builds a scene with BOTH in it, on purpose:
//
//     3 solid 4x4 squares  -> area 16 each   ("the parts")
//    12 single pixels      -> area  1 each   ("the noise")
//
// and drives blob_analysis's xi.pack@1 door TWICE on that same image:
//
//   RAW    min_area = 1        -> 15 blobs. The noise is really there.
//   GATED  min_area = gate(10) -> 3 blobs, whose centroids land exactly on the
//                                 three squares we drew.
//
// Both halves matter. A demo that only showed "3 blobs" would look identical if
// the specks had never been drawn — it is the RAW leg that proves the gate did
// the work.
//
// Two more things this shows, almost in passing:
//
//   * a parameter that changes per frame rides IN the pack (`min_area` here),
//     while the instance def in instances/det/instance.json is the fallback the
//     door uses when the pack does not carry one (`in.i64_or(key, def)`).
//   * how to actually READ the nested `blobs` array from a script — the mp walk
//     below is the part every real blob_analysis user has to write once.
//
// The image is synthesised in the script so the ground truth is exact. In a real
// project `gray` is your camera frame converted to single-channel u8 — nothing
// else about the call changes.
//
// Try it: drag `min_area_gate` up past 16 in the UI and the GATED leg drops to
// zero blobs — the gate will happily throw your parts away too.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cmath>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

// The knob the operator owns. Live-editable: the GATED leg re-runs with the new
// value on the very next tick, on the same synthetic scene.
xi::Param<int> gate{"min_area_gate", 10, xi::Range<int>{1, 400}};

namespace {

// ---- the scene ------------------------------------------------------------
// Key names are the plugin's contract keyset — spelled once in
// contract/codegen/generated/plugins/blob_analysis_keys.gen.h, which is what the
// plugin itself compiles against. Keep these strings in step with that header.
constexpr int W = 48, H = 32;
constexpr uint8_t FG = 200;              // foreground level (> threshold 128)
constexpr int SQ = 4;                    // 4x4 square -> area 16
constexpr int PART_XY[3][2] = { {6, 6}, {22, 10}, {36, 20} };
constexpr int N_SPECK = 12;              // 12 isolated single pixels -> area 1
constexpr int SPECK_Y[2] = { 2, 28 };    // two rows, clear of every square
constexpr int SPECK_X[6] = { 2, 10, 18, 26, 34, 42 };   // 8px apart: never touching

void paint(std::vector<uint8_t>& g) {
    g.assign((size_t)W * H, 0);
    for (auto& p : PART_XY)
        for (int y = 0; y < SQ; ++y)
            for (int x = 0; x < SQ; ++x)
                g[(size_t)(p[1] + y) * W + (p[0] + x)] = FG;
    for (int sy : SPECK_Y)
        for (int sx : SPECK_X)
            g[(size_t)sy * W + sx] = FG;
}

// ---- reading the nested `blobs` entry -------------------------------------
// blob_analysis returns the per-blob results as ONE canonical-msgpack entry:
//
//   [ { "area":16, "cx":7.5, "cy":7.5, "min_x":6, ..., "contour":[{x,y},...] },
//     ... ]
//
// Nesting is msgpack's job (a flattened "blob_0_cx" key convention would rot the
// moment a blob gained a field), so the consumer walks it with xi::mp::Reader.
struct Blob { long long area = 0; double cx = 0, cy = 0; };

// Consume exactly one value at the cursor, however deep. Reader::next() stops at
// the HEADER of an array/map and leaves the cursor on the first child, so the
// children have to be walked to get past it — that is all this does. We need it
// to step over the `contour` polygon we are not interested in here.
bool mp_skip(xi::mp::Reader& r) {
    xi::mp::Element e;
    if (r.next(e) != xi::mp::Status::Ok) return false;
    uint64_t n = 0;
    if (e.kind == xi::mp::Kind::Array)    n = e.len;
    else if (e.kind == xi::mp::Kind::Map) n = (uint64_t)e.len * 2;
    for (uint64_t i = 0; i < n; ++i)
        if (!mp_skip(r)) return false;
    return true;
}

bool read_blobs(std::span<const uint8_t> mp, std::vector<Blob>& out) {
    using xi::mp::Kind; using xi::mp::Status;
    xi::mp::Reader r(mp.data(), mp.size());
    xi::mp::Element top;
    if (r.next(top) != Status::Ok || top.kind != Kind::Array) return false;
    for (uint32_t i = 0; i < top.len; ++i) {
        xi::mp::Element m;
        if (r.next(m) != Status::Ok || m.kind != Kind::Map) return false;
        Blob b;
        for (uint32_t k = 0; k < m.len; ++k) {
            xi::mp::Element key;
            if (r.next(key) != Status::Ok || key.kind != Kind::Str) return false;
            const std::string_view ks((const char*)key.data, key.len);
            xi::mp::Element v;
            if (ks == "area") {
                if (r.next(v) != Status::Ok) return false;
                b.area = (v.kind == Kind::UInt) ? (long long)v.u : v.i;
            } else if (ks == "cx" || ks == "cy") {
                if (r.next(v) != Status::Ok) return false;
                (ks == "cx" ? b.cx : b.cy) = v.d;
            } else if (!mp_skip(r)) {     // bbox, contour_points, contour, ...
                return false;
            }
        }
        out.push_back(b);
    }
    return true;
}

// One door call: build {gray, threshold, min_area} and read the count back.
long long count_blobs(const std::vector<uint8_t>& gray, int min_area,
                      xi::ScriptPack& result_out) {
    xi::ScriptPackBuilder b;
    if (!b.add_image_blob("gray", W, H, 1, "u8", gray.data(), (int64_t)W * H)) return -1;
    b.add_i64("threshold", 128);      // per-pack parameter: wins over the def
    b.add_i64("min_area", min_area);  // THE knob this example is about
    auto in = b.seal();
    if (!in.valid()) return -1;

    result_out = xi::use("det").process(in);
    // A missing / mis-typed `gray` comes back as a NORMAL sealed pack carrying
    // $fault — never a silent zero. Check it before reading results.
    if (!result_out.valid() || result_out.is_fault()) return -1;
    return result_out.get_i64("blob_count").value_or(-1);
}

}  // namespace

XI_INSPECT_ENTRY(t, frame) {
    (void)t; (void)frame;

    std::vector<uint8_t> gray;
    paint(gray);

    // ---- leg 1: RAW — nothing gated. The noise is real and it counts. ------
    xi::ScriptPack raw_out;
    const long long raw = count_blobs(gray, 1, raw_out);

    // ---- leg 2: GATED — min_area filters the specks out. -------------------
    xi::ScriptPack gated_out;
    const long long gated = count_blobs(gray, (int)gate, gated_out);

    if (raw < 0 || gated < 0) {
        const auto why = gated_out.fault_reason().value_or(
                         raw_out.fault_reason().value_or("door returned nothing"));
        xi::ng(2, std::string("blob_analysis door failed: ").append(why).c_str());
        return;
    }

    // ---- the geometry, not just the count ----------------------------------
    // The plugin scans row-major, so the surviving blobs arrive in the order we
    // drew the squares. Every centroid must sit at the square's centre
    // (top-left + 1.5) to within rounding.
    std::vector<Blob> blobs;
    bool geom = false;
    if (auto mp = gated_out.get_mp("blobs")) {
        geom = read_blobs(*mp, blobs) && blobs.size() == 3;
        for (size_t i = 0; geom && i < blobs.size(); ++i)
            geom = blobs[i].area == SQ * SQ &&
                   std::fabs(blobs[i].cx - (PART_XY[i][0] + (SQ - 1) / 2.0)) < 0.01 &&
                   std::fabs(blobs[i].cy - (PART_XY[i][1] + (SQ - 1) / 2.0)) < 0.01;
    }

    // ---- surface it so the UI shows the gated binary, not just a number -----
    xi::ScriptPackBuilder e;
    e.add_str("$channel", "blobs");
    e.add_i64("$seq", (int64_t)xi::run_id());
    if (auto bin = gated_out.image_blob("binary"))
        e.add_image_blob("binary", bin->width, bin->height, bin->channels, "u8",
                         bin->payload.data(), (int64_t)bin->payload.size());
    e.add_i64("raw_blobs", raw);
    e.add_i64("gated_blobs", gated);
    e.add_i64("min_area_gate", (int)gate);
    for (size_t i = 0; i < blobs.size() && i < 3; ++i) {
        char kx[16], ky[16];
        std::snprintf(kx, sizeof kx, "cx%zu", i);
        std::snprintf(ky, sizeof ky, "cy%zu", i);
        e.add_f64(kx, blobs[i].cx);
        e.add_f64(ky, blobs[i].cy);
    }
    xi::use("expose").push(e.seal());

    char msg[192];
    std::snprintf(msg, sizeof msg,
                  "blob raw=%lld gated=%lld gate=%d geom=%d "
                  "(%d parts + %d specks drawn)",
                  raw, gated, (int)gate, geom ? 1 : 0, 3, N_SPECK);

    // The gate did its job only if BOTH halves hold: the specks were counted
    // when ungated, and only the parts survive the gate — with the right
    // geometry. Anything else is a defect worth looking at.
    if (raw == 3 + N_SPECK && gated == 3 && geom) xi::ok(1, msg);
    else                                          xi::ng(1, msg);
}
