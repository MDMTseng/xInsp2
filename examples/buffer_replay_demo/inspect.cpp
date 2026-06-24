// buffer_replay_demo — the hot-param re-inspect loop, now that replay is a
// plugin (buffer_replay) and not a host facility.
//
// Each LIVE frame is captured into the buffer_replay ring. The inspect applies a
// tunable `thresh` Param. To re-inspect the SAME frame after changing thresh,
// send the buffer instance an exchange: {"command":"replay_last"} — it re-emits
// the buffered record, this script runs again on it with the new thresh, and you
// see over_count change without the source grabbing a new frame.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

static xi::Param<int> thresh{"thresh", 100, {0, 255}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    if (!t.is_active()) return;

    const std::string src = t.primary_source();   // "src" live, "buffer" on replay
    auto img = t.image(src);
    if (img.empty()) return;

    // Capture only LIVE frames; a replayed frame (emitter == "buffer") must not
    // be re-buffered, or replays would pile up duplicates.
    if (src != "buffer")
        xi::use("buffer").process(xi::Record().image("img", img));

    // Frame-dependent, param-dependent metric: how many pixels exceed thresh.
    const int thr = thresh;
    int over = 0;
    const uint8_t* px = img.data();
    for (size_t i = 0, n = img.size(); i < n; ++i) if (px[i] > thr) ++over;

    VAR(thresh,     thr);
    VAR(over_count, over);
    VAR(replayed,   src == std::string("buffer"));
    VAR(source,     src);
}
