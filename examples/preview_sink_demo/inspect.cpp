// preview_sink_demo — surfacing script output AFTER the VAR macro was removed
// from core, into MULTIPLE expose CHANNELS a UI can tab between.
//
// There is no per-run data macro anymore. To view what the script computed,
// push a Record to the `expose` plugin under a channel id — the generic
// xi::use("expose").process(rec); no special header. The channel rides in the
// record under the reserved key "$channel"; the record's own key order IS the
// display order (no layout macro).
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>   // xi::ok / xi::ng (not in the xi.hpp umbrella)

#include <cstdlib>            // std::abs

static xi::Param<int> gain{"gain", 7, {0, 100}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    const int g     = gain;
    const int score = frame * 10 + g;        // frame- + param-dependent metric

    // A synthetic 320x240 image: horizontal gradient + a faint checker + a bright
    // vertical bar that moves with `frame`, so continuous mode shows live motion.
    const int W = 320, H = 240;
    xi::Image img(W, H, 1), inv(W, H, 1);
    const int bar = (frame * 9) % W;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        int v = (x * 255) / W;
        if (((x / 24) + (y / 24)) % 2 == 0) v = (v * 3) / 4;   // subtle checker
        if (std::abs(x - bar) < 7) v = 255;                    // moving bright bar
        uint8_t px = (uint8_t)v;
        img.data()[y * W + x] = px;
        inv.data()[y * W + x] = (uint8_t)(255 - px);
    }

    // The new output path: surface to TWO channels; the UI tabs between them.
    // Record fields append in order (images tagged by key), so the UI renders the
    // channel top-to-bottom: values, then the image. "$channel" selects the channel.
    xi::Record bright;
    bright.set("frame", frame).set("score", score).set("gain", g)
          .image("gradient", img)
          .image("thumb", img);            // SAME buffer → host compresses it once (dedup)
    bright.set("$channel", "bright");
    xi::use("expose").process(bright);

    xi::Record dark;
    dark.set("frame", frame).image("inverted", inv);   // a distinct image
    dark.set("$channel", "dark");
    xi::use("expose").process(dark);

    // The run's verdict still leaves on its own dedicated path.
    if (score >= 0) xi::ok(1, "ok"); else xi::ng(1, "neg");
}
