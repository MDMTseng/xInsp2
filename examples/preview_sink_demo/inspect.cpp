// preview_sink_demo — surfacing script output AFTER VAR was removed from core,
// into MULTIPLE preview groups (pg_id) a UI can tab between.
//
// VAR(name, value) still compiles but no longer publishes anything. To view what
// the script computed, push a Record to the preview_sink plugin under a pg_id.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>   // xi::ok / xi::ng (not in the xi.hpp umbrella)
#include "preview_api.hpp"    // xi::preview::Sink (ships with the preview_sink plugin)

static xi::Param<int> gain{"gain", 7, {0, 100}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    const int g     = gain;
    const int score = frame * 10 + g;        // frame- + param-dependent metric

    // A synthetic image + its inverse, so we exercise the image path (no camera).
    xi::Image img(8, 8, 1), inv(8, 8, 1);
    for (size_t i = 0, n = img.size(); i < n; ++i) {
        img.data()[i] = (uint8_t)((score + (int)i) & 0xFF);
        inv.data()[i] = (uint8_t)(255 - img.data()[i]);
    }

    // The new output path: surface to TWO preview groups; the UI tabs between them.
    xi::preview::Sink pv;
    pv.process("bright", xi::Record().set("score", score).set("gain", g).image("synth", img));
    pv.process("dark",   xi::Record().set("score", 255 - score).image("inv", inv));

    // The run's verdict still leaves on its own channel (unaffected by VAR removal).
    if (score >= 0) xi::ok(1, "ok"); else xi::ng(1, "neg");
}
