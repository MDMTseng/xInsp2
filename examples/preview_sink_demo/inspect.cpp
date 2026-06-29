// preview_sink_demo — surfacing script output AFTER VAR was removed from core,
// into MULTIPLE preview groups (pg_id) a UI can tab between.
//
// VAR(name, value) still compiles but no longer publishes anything. To view what
// the script computed, push a Record to the preview_sink plugin under a pg_id.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>   // xi::ok / xi::ng (not in the xi.hpp umbrella)
#include <xi/xi_preview.hpp>   // the `preview` plugin's script API (SDK)

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

    // The new output path: surface to TWO preview groups; the UI tabs between them.
    // PVAR() appends each field in order (and tags images), so the UI renders the
    // group top-to-bottom: values, then the image.
    xi::preview::Sink pv;

    xi::Record bright;
    PVAR(bright, "frame", frame);
    PVAR(bright, "score", score);
    PVAR(bright, "gain",  g);
    PVAR(bright, "gradient", img);
    PVAR(bright, "thumb",    img);   // SAME buffer → host compresses it once (dedup)
    pv.process("bright", bright);

    xi::Record dark;
    PVAR(dark, "frame", frame);
    PVAR(dark, "inverted", inv);     // a distinct image
    pv.process("dark", dark);

    // The run's verdict still leaves on its own channel (unaffected by VAR removal).
    if (score >= 0) xi::ok(1, "ok"); else xi::ng(1, "neg");
}
