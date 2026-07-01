// buffer_replay_demo — the hot-param re-inspect loop, with replay as a plugin
// (the `cache` plugin, instance "buffer") rather than a host facility.
//
// Each LIVE frame is captured into the `cache` ring. The inspect applies a
// tunable `thresh` Param. To re-inspect the SAME frame after changing thresh,
// send the buffer instance an exchange {"command":"replay_last"} — it re-emits
// the buffered record, this script runs again on it with the new thresh, and
// over_count changes WITHOUT the source grabbing a new frame.
//
// VAR was removed from core; results are pushed to the `expose` sink on channel
// "runs" (a driver/UI subscribes or pulls the latest to observe them).
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include <string>

static xi::Param<int> thresh{"thresh", 100, {0, 255}};

// A4: explicit-trigger entry — the host passes the trigger in (no ambient
// thread_local), so `t` is self-contained.
XI_INSPECT_ENTRY(t, /*frame*/ frame) {
    (void)frame;
    if (!t.is_active()) return;

    const std::string src = t.primary_source();   // "src" live, "buffer" on replay
    auto img = t.image(src);
    if (img.empty()) return;

    // Capture only LIVE frames; a replayed frame (emitter == "buffer") must not
    // be re-buffered, or replays would pile up duplicates in the ring.
    if (src != "buffer")
        xi::use("buffer").process(xi::Record().image("img", img));

    // Frame-dependent, param-dependent metric: how many pixels exceed thresh.
    const int thr = thresh;
    int over = 0;
    const uint8_t* px = img.data();
    for (size_t i = 0, n = img.size(); i < n; ++i) if (px[i] > thr) ++over;

    // Publish to the expose sink (channel "runs") — the driver reads these to
    // watch over_count move as thresh changes across replays of the SAME frame.
    xi::Record rec;
    rec.set("over",     (int64_t)over)
       .set("thresh",   (int64_t)thr)
       .set("replayed", src == std::string("buffer"))
       .set("source",   src)
       .set("$channel", "runs");
    xi::use("expose").process(rec);
}
