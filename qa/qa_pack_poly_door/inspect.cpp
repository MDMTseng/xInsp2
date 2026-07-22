// qa_pack_poly_door — the POLYMORPHIC DOOR pattern: ONE plugin door
// (poly_door's xi.pack@1 door) dispatches different behaviors on the pack's
// producer-domain `type` entry — the same idiom as the capability plane's
// `$v` dispatch (provider-internal; the funnel forwards blind), blessed here
// on a BARE `type` key (NOT `$type`: the `$` namespace is framework-reserved
// — see contract/canonical-profile-notes.md "Producer-domain type dispatch").
//
// Per tick (synthetic timer drives — no camera needed) the script routes a
// MIXED sequence through the SAME door via xi::use("door").process():
//
//   1. type="measure"  -> stats over the fixed 16x16 image (5x5 white square):
//                         pixels=256 nonzero=25 min=0 max=255 mean=6375/256.
//   2. type="annotate" -> derived overlay entry: 0/255 mask of pixels > 128,
//                         marked=25, byte-checked at (7,7) white / (0,0) black.
//   3. type="rotate"   -> UNKNOWN: a sealed $fault with reason
//                         unsupported_type, $fault_key="type", and the
//                         supported set named in "types" — the producer-domain
//                         mirror of the capability plane's $versions idiom.
//
// Bonus (aggregation): a running per-type dispatch count accumulates in
// xi::kv() (km/ka/ku) and rides out in every verdict message; the driver
// asserts the counts stay equal and track seq exactly.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

XI_INSPECT_ENTRY(t, frame) {
    (void)frame; (void)t;
    const long long seq = xi::kv().get_i64("ticks", 0);
    xi::kv().set_i64("ticks", seq + 1);

    // ---- the fixed synthetic image: 16x16 gray, 5x5 white square ----------
    constexpr int W = 16, H = 16;
    std::vector<uint8_t> gray((size_t)W * H, 0);
    for (int y = 5; y < 10; ++y)
        for (int x = 5; x < 10; ++x) gray[(size_t)y * W + x] = 255;

    bool built = true;

    // ---- 1. type="measure" through the door --------------------------------
    xi::ScriptPackBuilder mb;
    built = mb.valid() && built;
    built = mb.add_str("type", "measure") && built;
    built = mb.add_image_blob("gray", W, H, 1, "u8", gray.data(), (int64_t)W * H * 1) && built;
    auto mpk = mb.seal();
    built = built && mpk.valid();
    auto mo = xi::use("door").process(mpk);
    const double mean  = mo.get_f64("mean").value_or(-1.0);
    const bool   m_ok  = mo.valid() && !mo.is_fault() &&
                         mo.get_str("type").value_or("") == "measure" &&
                         mo.get_i64("pixels").value_or(-1) == W * H &&
                         mo.get_i64("nonzero").value_or(-1) == 25 &&
                         mo.get_i64("min").value_or(-1) == 0 &&
                         mo.get_i64("max").value_or(-1) == 255 &&
                         std::fabs(mean - 6375.0 / 256.0) < 1e-9 &&
                         mo.src().value_or("") == "door";   // auto-stamped hop
    if (m_ok) xi::kv().set_i64("n_measure", xi::kv().get_i64("n_measure", 0) + 1);

    // ---- 2. type="annotate" through the SAME door ---------------------------
    xi::ScriptPackBuilder ab;
    built = ab.valid() && built;
    built = ab.add_str("type", "annotate") && built;
    built = ab.add_i64("threshold", 128) && built;
    built = ab.add_image_blob("gray", W, H, 1, "u8", gray.data(), (int64_t)W * H * 1) && built;
    auto ain = ab.seal();
    built = built && ain.valid();
    auto ao = xi::use("door").process(ain);
    auto ov = ao.image_blob("overlay");
    const bool a_ok = ao.valid() && !ao.is_fault() &&
                      ao.get_str("type").value_or("") == "annotate" &&
                      ao.get_i64("threshold_used").value_or(-1) == 128 &&
                      ao.get_i64("marked").value_or(-1) == 25 &&
                      ov && ov->width == W && ov->height == H &&
                      ov->channels == 1 &&
                      ov->payload[(size_t)7 * W + 7] == 255 &&   // inside square
                      ov->payload[0] == 0;                        // outside
    if (a_ok) xi::kv().set_i64("n_annotate", xi::kv().get_i64("n_annotate", 0) + 1);

    // ---- 3. UNKNOWN type through the SAME door: the fault leg ---------------
    xi::ScriptPackBuilder ub;
    built = ub.valid() && built;
    built = ub.add_str("type", "rotate") && built;
    built = ub.add_image_blob("gray", W, H, 1, "u8", gray.data(), (int64_t)W * H * 1) && built;
    auto uin = ub.seal();
    built = built && uin.valid();
    auto uo = xi::use("door").process(uin);
    const bool u_ok = uo.valid() && uo.is_fault() &&
                      uo.fault_reason().value_or("") == "unsupported_type" &&
                      uo.fault_key().value_or("") == "type" &&
                      uo.get_str("types").value_or("") == "measure,annotate";
    if (u_ok) xi::kv().set_i64("n_unknown", xi::kv().get_i64("n_unknown", 0) + 1);

    // ---- verdict: dispatch legs + the kv() per-type running counts ----------
    const long long km = xi::kv().get_i64("n_measure", 0);
    const long long ka = xi::kv().get_i64("n_annotate", 0);
    const long long ku = xi::kv().get_i64("n_unknown", 0);
    const bool pass = built && m_ok && a_ok && u_ok &&
                      km == seq + 1 && ka == seq + 1 && ku == seq + 1;
    char msg[160];
    std::snprintf(msg, sizeof msg,
                  "polydoor seq=%lld built=%d m=%d a=%d u=%d km=%lld ka=%lld "
                  "ku=%lld",
                  seq, built ? 1 : 0, m_ok ? 1 : 0, a_ok ? 1 : 0, u_ok ? 1 : 0,
                  km, ka, ku);
    if (pass) xi::ok(1, msg);
    else      xi::ng(1, msg);
}
