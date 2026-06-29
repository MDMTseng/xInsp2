// preview_sink_demo — surfacing script output AFTER VAR was removed from core.
//
// VAR(name, value) still compiles but no longer publishes anything. To view what
// the script computed, push a Record to the preview_sink plugin instead. A UI /
// test then pulls the latest via exchange_instance({"command":"get_latest"}).
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>   // xi::ok / xi::ng (not in the xi.hpp umbrella)

static xi::Param<int> gain{"gain", 7, {0, 100}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    const int g     = gain;
    const int score = frame * 10 + g;        // some frame- + param-dependent metric

    // A small synthetic image so we exercise the image path too (no camera needed).
    xi::Image img(8, 8, 1);
    for (size_t i = 0, n = img.size(); i < n; ++i)
        img.data()[i] = (uint8_t)((score + (int)i) & 0xFF);

    // The new output path: surface to the preview plugin (VAR is a dormant stub).
    xi::use("preview").process(
        xi::Record()
            .set("score", score)
            .set("gain",  g)
            .image("synth", img));

    // The run's verdict still leaves on its own channel (unaffected by VAR removal).
    if (score >= 0) xi::ok(1, "ok"); else xi::ng(1, "neg");
}
