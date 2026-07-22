// mock_camera example — auto-exposure: the script drives the camera back.
//
// A source plugin is usually thought of as one-way: it emits, you inspect. It
// is not. mock_camera also has a pack DOOR, so the same script that measures
// the frame can push a correction straight back into the camera. That closes
// the loop with nothing but the pack plane — no side channel, no new API.
//
// The loop, once per trigger:
//
//   SENSE    t.pack() carries the frame AND the gain it was painted with. The
//            camera echoes its own state per frame, so the script never has to
//            guess what setting produced the pixels it is holding.
//   ANALYZE  mean intensity over the frame.
//   DECIDE   gain * (target / mean) — a proportional correction computed
//            against THIS frame's gain, which is what keeps the law stable
//            while an earlier command is still in flight.
//   ACTUATE  a {command:"set_gain", value:...} pack into cam's own door. The
//            sealed reply IS the ack.
//
// The camera starts at gain 0.2 — deliberately far too dim. Watch `mean` climb
// into the target band over the first handful of frames and stay there.
//
// The new gain lands on the NEXT emitted frame. That one-frame latency is the
// contract; this is frame-rate regulation, not a sub-frame servo.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>

// Live-tunable from the UI: drag it and the loop re-converges on the new target.
xi::Param<int> target{"target_mean", 110, xi::Range<int>{20, 220}};

static constexpr double kBand   = 14.0;   // "converged" = |mean - target| <= 14
static constexpr double kGainLo = 0.05;   // mirror the plant's own clamp, so the
static constexpr double kGainHi = 8.0;    // script never commands the impossible

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    // ---- SENSE -------------------------------------------------------------
    auto tp = t.pack();
    if (!tp) return;
    const long long seq  = (long long)tp.get_i64("seq").value_or(-1);
    const double    gain = tp.get_f64("gain").value_or(-1.0);
    auto img = tp.image_blob("frame");
    if (!img || img->payload.empty()) { xi::ng(1, "no frame image on the pack"); return; }

    // ---- ANALYZE -----------------------------------------------------------
    unsigned long long sum = 0;
    for (uint8_t v : img->payload) sum += v;
    const double mean = (double)sum / (double)img->payload.size();

    // ---- DECIDE ------------------------------------------------------------
    const double tgt = (double)target;
    double cmd = gain * (tgt / (mean > 1.0 ? mean : 1.0));
    if (cmd < kGainLo) cmd = kGainLo; else if (cmd > kGainHi) cmd = kGainHi;

    // ---- ACTUATE: a control pack into the camera's OWN door -----------------
    xi::ScriptPackBuilder cb;
    cb.add_str("command", "set_gain");
    cb.add_f64("value", cmd);
    auto ack = xi::use("cam").process(cb.seal());
    auto a   = ack.get_str("ack");
    const bool ack_ok = ack.valid() && !ack.is_fault() && a && *a == "set_gain";

    // Surface the frame + the loop's own state so the webUI shows the settling
    // curve, not just the final number.
    xi::ScriptPackBuilder e;
    e.add_str("$channel", "camera");
    e.add_i64("$seq", (int64_t)xi::run_id());
    e.add_image("frame", img->width, img->height, img->channels, img->payload.data());
    e.add_f64("mean", mean);
    e.add_f64("gain", gain);
    e.add_f64("commanded_gain", cmd);
    xi::use("view").push(e.seal());

    const bool converged = mean >= tgt - kBand && mean <= tgt + kBand;

    char msg[160];
    std::snprintf(msg, sizeof msg, "seq=%lld mean=%.1f gain=%.3f -> %.3f%s",
                  seq, mean, gain, cmd, converged ? " [in band]" : "");
    if (!ack_ok)        xi::ng(1, "camera door rejected the gain command");
    else if (converged) xi::ok(1, msg);
    else                xi::result(0, msg);   // still settling — not a defect
}
