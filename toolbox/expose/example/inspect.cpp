// expose example — the script's data-out surface, and what SUBSCRIPTION buys.
//
// A script has exactly one way to show a human anything: it builds a sealed
// pack and pushes it at an `expose` instance. There is no VAR table, no image
// register, no side channel. `xi::use("expose").push(pack)` is the whole API.
//
// The two ideas this file is here to teach:
//
//   1. CHANNELS. The reserved key "$channel" names the lane a record belongs
//      to. expose keeps the LATEST record per channel and the webUI turns each
//      channel into its own tab. So a script does not push "the output" — it
//      pushes `measure` (numbers, tiny, always interesting) and `detail`
//      (pixels, fat, interesting only when someone is looking). Naming the
//      lanes is what makes that separation possible at all.
//
//   2. SUBSCRIPTION GATES THE PUSH, NOT THE RECORD. Both channels below are
//      pushed on every single run. expose STORES both every time. But it only
//      JPEG-encodes and broadcasts a channel that some client has explicitly
//      `subscribe`d to. An unwatched `detail` channel costs a store and
//      nothing else — no encode, no bytes on the socket. This is why you can
//      leave a fat debug channel in a production script.
//
//      The pull views are the other half of that deal: `list_channels` shows
//      every channel that has ever been written (subscribed or not), and `get`
//      base64s the latest frame of ANY channel on demand. Nothing is lost by
//      not subscribing — it is just not streamed at you.
//
// Try it: open the webUI, watch `measure` tick while `detail` sits idle, click
// the `detail` tab, and watch the images start flowing.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>
#include <vector>

// Live-tunable from the UI. The mask on the `detail` channel repaints as you
// drag it — a Param plus an exposed image is the whole "tune it by eye" loop.
xi::Param<int> threshold{"threshold", 128, xi::Range<int>{0, 255}};

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    // ---- the input: mock_camera's frame, off the trigger pack --------------
    auto tp = t.pack();
    if (!tp) return;
    auto img = tp.image_blob("frame");
    if (!img || img->payload.empty()) { xi::ng(1, "no frame image on the pack"); return; }

    const long long seq = (long long)tp.get_i64("seq").value_or(-1);
    const int w = img->width, h = img->height;

    // ---- analyze: one pass for the numbers AND the mask ---------------------
    const int thr = (int)threshold;
    unsigned long long sum = 0;
    long long          hits = 0;
    uint8_t            lo = 255, hi = 0;
    std::vector<uint8_t> mask(img->payload.size(), 0);
    for (size_t i = 0; i < img->payload.size(); ++i) {
        const uint8_t v = img->payload[i];
        sum += v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        if (v >= thr) { mask[i] = 255; ++hits; }
    }
    const double mean = (double)sum / (double)img->payload.size();
    const double bright_pct = 100.0 * (double)hits / (double)img->payload.size();

    // ---- CHANNEL 1: "measure" — numbers only, cheap, always worth pushing ---
    // No image entry at all. Even when subscribed this frame is a few hundred
    // bytes, so it is the channel you leave streaming to a dashboard.
    {
        xi::ScriptPackBuilder m;
        m.add_str("$channel", "measure");        // <- the lane name. THE key.
        m.add_i64("$seq", (int64_t)xi::run_id()); // host ordering stamp
        m.add_i64("seq", seq);
        m.add_f64("mean", mean);
        m.add_i64("min", (int64_t)lo);
        m.add_i64("max", (int64_t)hi);
        m.add_i64("threshold", (int64_t)thr);
        m.add_f64("bright_pct", bright_pct);
        xi::use("expose").push(m.seal());
    }

    // ---- CHANNEL 2: "detail" — the pixels, fat, pushed only when watched ----
    // TWO images in ONE record: a record is a bundle, not a single picture, so
    // the source frame and the derived mask arrive together and can never be
    // shown a frame apart. Encoding both costs real CPU — which is exactly the
    // cost expose skips while nobody is subscribed to this channel.
    {
        xi::ScriptPackBuilder d;
        d.add_str("$channel", "detail");
        d.add_i64("$seq", (int64_t)xi::run_id());
        d.add_i64("seq", seq);
        d.add_i64("threshold", (int64_t)thr);
        d.add_image("frame", w, h, img->channels, img->payload.data());
        d.add_image("mask",  w, h, img->channels, mask.data());
        xi::use("expose").push(d.seal());
    }

    // The verdict plane is separate from the data plane: run_result carries the
    // pass/fail, expose carries what you look at. Neither replaces the other.
    char msg[160];
    std::snprintf(msg, sizeof msg,
                  "seq=%lld mean=%.1f thr=%d bright=%.1f%% (2 channels pushed)",
                  seq, mean, thr, bright_pct);
    xi::ok(1, msg);
}
