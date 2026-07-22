// ui_egress example — the live view is not your problem.
//
// Every vision app eventually grows the same bug: somebody puts the operator's
// live view on the inspection path. Now a 30fps camera encodes 30 JPEGs a
// second whether or not anyone is looking at the screen, and a slow WS client
// backs pressure up into the thing that is supposed to be measuring parts.
//
// `ui_egress` is a LIB plugin that takes that job away. It has no data plane;
// it registers ONE capability, xi.ui.egress, whose whole contract is:
//
//     write a latest-wins retained slot for this channel, and RETURN.
//
// No encode, no fan-out, no blocking — those happen later, on the plugin's own
// timer thread, at the UI's rate (`fps: 5` here), not the camera's. Anything
// that arrives between flushes is simply overwritten: a live view is a view of
// NOW, so the stale frame has no value worth queueing.
//
// Here the producer is the camera itself (`ui_preview: true` in
// instances/cam), which is why you will not find the live view in this file.
// One line inside mock_camera's capture loop pushes each painted frame to the
// capability, and that is the entire integration. This script does the actual
// job — measure the part, return a verdict — and never learns that a UI
// exists. The two planes only meet inside `view`.
//
// Three properties worth watching, all asserted by driver.py:
//
//   * NOBODY WATCHING COSTS NOTHING. Before a client subscribes, egress probes
//     the sink, sees zero subscribers, and drops at the probe. `encodes` stays
//     0 — the camera pushed hundreds of frames and not one was compressed.
//   * THE UI RATE WINS. A 30fps source is delivered at ~5fps. The slot
//     collapses the difference; the producer never waits for it.
//   * NO EGRESS, NO PROBLEM. Remove the provider and the push becomes a no-op
//     inside the camera. The live view goes away. The product plane below —
//     this script's frames and verdicts — is byte-for-byte what it was.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>

// The inspection's own threshold, so this reads like a real script and not a
// transport demo.
xi::Param<int> min_mean{"min_mean", 20, xi::Range<int>{0, 255}};

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    auto p = t.pack();
    if (!p) return;
    const long long seq = (long long)p.get_i64("seq").value_or(-1);
    auto img = p.get_image("frame");
    if (!img || img->pixels.empty() || seq < 0) { xi::ng(1, "no frame"); return; }

    unsigned long long sum = 0;
    for (uint8_t v : img->pixels) sum += v;
    const int mean = (int)(sum / img->pixels.size());

    // The PRODUCT plane: the record this inspection is accountable for. It goes
    // to `view` on channel "cam" — a different channel from the live view's
    // "ui/cam", produced by different code, on a different thread, at a
    // different rate. Nothing here is throttled or dropped for the UI's sake.
    xi::ScriptPackBuilder b;
    b.add_str("$channel", "cam");
    b.add_i64("$seq", seq);
    b.add_i64("seq", seq);
    b.add_i64("w", img->width);
    b.add_i64("h", img->height);
    b.add_i64("mean", mean);
    b.add_image("img", img->width, img->height, img->channels, img->pixels.data());
    auto out = b.seal();
    if (out.valid()) xi::use("view").push(out);

    char msg[96];
    std::snprintf(msg, sizeof msg, "seq=%lld mean=%d", seq, mean);
    if (mean >= min_mean) xi::ok(1, msg);
    else                  xi::ng(1, msg);
}
