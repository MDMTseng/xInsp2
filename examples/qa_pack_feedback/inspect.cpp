// qa_pack_feedback — CLOSED-LOOP CONTROL AT FRAME LATENCY, pack-only.
//
// The maintainer-settled claim this example proves: because a SOURCE plugin
// can also expose a pack door (the bilingual source, both directions),
// analysis -> actuation loops are expressible TODAY with nothing but the
// existing pack plane — no new plane, no side channel, no Record.
//
// mock_camera (PACK MODE, initial gain 0.2 — deliberately dim) drives the
// pipeline. Per trigger the script:
//
//   1. SENSE   — t.pack(): the emitted frame + the gain it was painted with
//                (the plant echoes its own state per frame).
//   2. ANALYZE — mean intensity over every pixel/channel of the frame.
//   3. DECIDE  — proportional multiplicative correction toward the target
//                band: cmd = gain * target/mean, clamped like the plant knob.
//                Computed against THIS frame's echoed gain, so the law stays
//                self-consistent even when a previous command is still in
//                flight (the one-frame-latency race is benign by design).
//   4. ACTUATE — a CONTROL pack {command:"set_gain", value:cmd} pushed into
//                the camera's OWN xi.pack@1 door via xi::use("cam").process();
//                the sealed door output is the ack (clamped gain echoed).
//
// The commanded gain takes effect on the NEXT emitted frame — that is the
// frame-latency contract (see README.md; sub-frame loops are out of scope,
// docs/new_gen/07-uniform-keyed-buffer-plane.md). Over a handful of frames the
// measured mean converges monotonically into the target band and stays there
// while the loop keeps regulating against the source's own gradient drift.
//
// Verdicts ride the run_result plane (xi::ok/ng); the driver asserts the
// convergence envelope. No xi::Record anywhere.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>

static constexpr double kTarget = 110.0;  // target mean intensity
static constexpr double kBand   = 14.0;   // acceptance band: |mean-target| <= 14
static constexpr double kGainLo = 0.05;   // mirror of the plant's clamp
static constexpr double kGainHi = 8.0;

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    // ---- 1. SENSE: the pack frame + the plant's per-frame state echo -------
    auto tp = t.pack();
    if (!tp) return;                                  // no pack on this tick → NA
    const long long seq  = (long long)tp.get_i64("seq").value_or(-1);
    const double    gain = tp.get_f64("gain").value_or(-1.0);
    auto img = tp.get_image("frame");
    if (!img || img->pixels.empty()) { xi::ng(1, "fb: no frame image on the pack"); return; }

    // ---- 2. ANALYZE: mean intensity across the whole frame ------------------
    unsigned long long sum = 0;
    for (uint8_t v : img->pixels) sum += v;
    const double mean = (double)sum / (double)img->pixels.size();

    // ---- 3. DECIDE: proportional multiplicative correction ------------------
    double cmd = gain * (kTarget / (mean > 1.0 ? mean : 1.0));
    if (cmd < kGainLo) cmd = kGainLo; else if (cmd > kGainHi) cmd = kGainHi;

    // ---- 4. ACTUATE: a control pack into the camera's OWN door --------------
    xi::ScriptPackBuilder cb;
    bool built = cb.valid();
    built = cb.add_str("command", "set_gain") && built;
    built = cb.add_f64("value", cmd) && built;
    auto ctrl = cb.seal();

    bool   ack_ok   = false;
    double ack_gain = -1.0;
    if (built && ctrl.valid()) {
        auto ack = xi::use("cam").process(ctrl);      // request-reply: the ack pack
        auto a   = ack.get_str("ack");
        ack_ok   = ack.valid() && !ack.is_fault() && a && *a == "set_gain";
        ack_gain = ack.get_f64("gain").value_or(-1.0);
    }

    const bool in_band = mean >= kTarget - kBand && mean <= kTarget + kBand;
    const bool pass    = seq >= 0 && gain > 0.0 && built && ack_ok;

    char msg[160];
    std::snprintf(msg, sizeof msg,
                  "fb seq=%lld mean=%.2f gain=%.4f cmd=%.4f ackg=%.4f band=%d",
                  seq, mean, gain, cmd, ack_gain, in_band ? 1 : 0);
    if (pass) xi::ok(1, msg);
    else      xi::ng(1, msg);
}
