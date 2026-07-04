// qa_pack_param_modulation — the per-frame PARAMETER-MODULATION teaching
// example: parameters that change at frame rate belong IN the pack (data
// plane), not in defs (control plane — commit_group is quiesced, hundred-ms
// scale). See this example's README.md for the three-cadence doctrine.
//
// Per trigger (mock_camera in PACK MODE drives the pipeline) the script:
//
//   1. Builds a STEPPED image with xi::ScriptPackBuilder: four 4x4 squares at
//      intensities 60/110/160/210 on black. blob_analysis binarizes with
//      `pixel > threshold`, so the blob count is a staircase function of the
//      threshold: thr=40 -> 4 blobs, 90 -> 3, 140 -> 2, 190 -> 1, 240 -> 0.
//   2. LEG A (data cadence): sweeps `threshold` ACROSS FRAMES — thr =
//      SWEEP[seq % 5] rides as a pack entry INTO blob_analysis's xi.pack@1
//      door (in.i64_or(keys::kThreshold, def) — per-pack entry wins). Asserts
//      the result reflects THIS frame's own parameter: blob_count ==
//      EXPECT[seq % 5] and threshold_used echoes the swept value.
//   3. LEG B (configuration cadence): the SAME frame, the SAME door, but with
//      NO threshold entry in the pack — the door falls through to the def
//      layer, set once by instances/det/instance.json { "threshold": 200 }.
//      Expects blob_count == 1 and threshold_used == 200 on EVERY frame.
//      200 != the plugin's compiled-in default (128, which would count 2), so
//      this leg proves the def actually landed AND stays constant while leg A
//      modulates per frame.
//
//      LEG B IS ALSO THE NO-LEAK PROOF: it runs AFTER leg A in the same tick.
//      The sealed-pack-per-call model makes leakage structural nonsense — a
//      pack entry lives only in the pack it was sealed into — but we assert
//      it anyway: if frame N's swept threshold ever leaked into the instance,
//      leg B would echo the swept value instead of 200.
//
//   4. Pushes the per-frame evidence (seq, thr, both legs' counts and echoed
//      thresholds) to expose channel "qa"; the driver reconstructs the
//      staircase from the wire and re-asserts everything frame by frame.
//
// No xi::Record anywhere; blob is driven purely through its xi.pack@1 door.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>
#include <vector>

namespace {
constexpr int W = 24, H = 24;
constexpr int SQ = 4;                             // 4x4 = area 16 > min_area 10
constexpr int LEVELS[4]  = {60, 110, 160, 210};   // square intensities
constexpr int OFFS[4][2] = {{2, 2}, {14, 2}, {2, 14}, {14, 14}};
constexpr int SWEEP[5]   = {40, 90, 140, 190, 240};  // per-frame thresholds
constexpr int EXPECT[5]  = {4, 3, 2, 1, 0};          // levels strictly above thr
constexpr int DEF_THR    = 200;   // instances/det/instance.json (NOT the 128 built-in)
constexpr int DEF_COUNT  = 1;     // levels strictly above 200: {210}
}  // namespace

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    auto tp = t.pack();
    if (!tp) return;                                 // no pack on this tick -> NA
    const long long seq = (long long)tp.get_i64("seq").value_or(-1);
    const int step = seq >= 0 ? (int)(seq % 5) : 0;
    const int thr  = SWEEP[step];
    const int want = EXPECT[step];

    // ---- the stepped image: a predictable blob-count staircase -------------
    std::vector<uint8_t> gray((size_t)W * H, 0);
    for (int s = 0; s < 4; ++s)
        for (int y = 0; y < SQ; ++y)
            for (int x = 0; x < SQ; ++x)
                gray[(size_t)(OFFS[s][1] + y) * W + (OFFS[s][0] + x)] =
                    (uint8_t)LEVELS[s];

    // ---- LEG A: the per-frame parameter rides IN the pack (data cadence) ---
    xi::ScriptPackBuilder ba;
    bool built = ba.valid();
    built = ba.add_image("gray", W, H, 1, gray.data()) && built;
    built = ba.add_i64("threshold", thr) && built;   // THE modulated parameter
    auto in_a = ba.seal();
    built = built && in_a.valid();

    auto out_a = xi::use("det").process(in_a);
    const long long blobs_a = (long long)out_a.get_i64("blob_count").value_or(-1);
    const long long thru_a  = (long long)out_a.get_i64("threshold_used").value_or(-1);
    const bool fault_a = out_a.get_str("$fault").has_value();
    const bool leg_a_ok = out_a.valid() && !fault_a &&
                          blobs_a == want && thru_a == thr;

    // ---- LEG B: NO threshold entry -> the def layer (config cadence) -------
    // Runs AFTER leg A: if A's per-pack threshold could leak into the
    // instance, this call would echo it. It must echo the instance.json def.
    xi::ScriptPackBuilder bb;
    bool built_b = bb.valid();
    built_b = bb.add_image("gray", W, H, 1, gray.data()) && built_b;
    auto in_b = bb.seal();
    built_b = built_b && in_b.valid();

    auto out_b = xi::use("det").process(in_b);
    const long long blobs_b = (long long)out_b.get_i64("blob_count").value_or(-1);
    const long long thru_b  = (long long)out_b.get_i64("threshold_used").value_or(-1);
    const bool fault_b = out_b.get_str("$fault").has_value();
    const bool leg_b_ok = out_b.valid() && !fault_b &&
                          blobs_b == DEF_COUNT && thru_b == DEF_THR;

    // ---- push the per-frame evidence for the driver's staircase ------------
    xi::ScriptPackBuilder rb;
    bool rbuilt = rb.valid();
    rbuilt = rb.add_str("$channel", "qa") && rbuilt;
    rbuilt = rb.add_i64("$seq", seq) && rbuilt;
    rbuilt = rb.add_i64("seq", seq) && rbuilt;
    rbuilt = rb.add_i64("thr", thr) && rbuilt;
    rbuilt = rb.add_i64("blobs", blobs_a) && rbuilt;
    rbuilt = rb.add_i64("thr_used", thru_a) && rbuilt;
    rbuilt = rb.add_i64("base_blobs", blobs_b) && rbuilt;
    rbuilt = rb.add_i64("base_thr_used", thru_b) && rbuilt;
    auto result = rb.seal();
    const bool pushed = rbuilt && result.valid() && xi::use("expose").push(result);

    const bool pass = seq >= 0 && built && built_b && leg_a_ok && leg_b_ok && pushed;
    char msg[192];
    std::snprintf(msg, sizeof msg,
                  "pmod seq=%lld thr=%d blobs=%lld want=%d thru=%lld "
                  "base=%lld base_thru=%lld faults=%d%d pushed=%d",
                  seq, thr, blobs_a, want, thru_a, blobs_b, thru_b,
                  fault_a ? 1 : 0, fault_b ? 1 : 0, pushed ? 1 : 0);
    if (pass) xi::ok(1, msg);
    else      xi::ng(1, msg);
}
