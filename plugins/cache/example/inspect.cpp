// cache example — the hot-param loop: buffer a frame, retune, re-inspect it.
//
// The camera is not the only way to get a frame to the script. `buffer` (the
// cache plugin) retains each sealed pack in a bounded ring; an exchange command
// re-emits one with a fresh trigger id, so the script runs again on the SAME
// pixels — no re-grab. That is what lets you drag a threshold and watch the
// verdict change on the frame you are looking at.
//
// Try it: let it run, stop the camera, then send the buffer a replay:
//
//     exchange_instance("buffer", {"command":"replay_last"})
//
// (the shipped driver.py does exactly that). A new run appears with the camera
// stopped — that run is the replay.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

// A tunable — the thing you would be retuning while replaying the same frame.
// Its value is live-editable from the UI; replay re-runs this script, so a new
// value takes effect on the buffered frame immediately.
xi::Param<int> bright_limit{"bright_limit", 128, xi::Range<int>{0, 255}};

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;

    auto p = t.pack();
    auto img = p.get_image("frame");
    if (!img) { xi::result(0, "no frame in this trigger"); return; }

    // Retain THIS pack in the ring. Cheap: the ring holds a reference to the
    // sealed pack, it does not copy pixels.
    xi::use("buffer").process(p);

    // A trivial "inspection" whose answer depends on the tunable, so a replay
    // with a different bright_limit gives a different verdict on identical
    // pixels — the point of the loop.
    const uint8_t* px = img->pixels.data();
    const size_t n = (size_t)img->width * img->height * img->channels;
    unsigned long long sum = 0;
    for (size_t i = 0; i < n; ++i) sum += px[i];
    const int mean = n ? (int)(sum / n) : 0;

    xi::ScriptPackBuilder e;
    e.add_str("$channel", "cache");
    e.add_i64("$seq", (int64_t)xi::run_id());
    e.add_image("frame", img->width, img->height, img->channels, img->pixels.data());
    e.add_i64("mean", mean);
    e.add_i64("bright_limit", bright_limit);
    xi::use("expose").push(e.seal());

    if (mean <= bright_limit) xi::ok(1, "mean within bright_limit");
    else                      xi::ng(1, "mean over bright_limit");
}
