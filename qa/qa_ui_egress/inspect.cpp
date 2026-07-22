// qa_ui_egress — the LIVE-UI egress pipeline e2e (spec 31 /
// docs/internals/ui-egress.md).
//
// The live-UI plane is DRIVEN BY THE PRODUCER, not this script: mock_camera
// (config ui_preview=on) pushes each painted frame to the xi.ui.egress cap on
// its own channel `ui/<instance>` — a one-line resolved-cap call inside its
// capture loop, entirely independent of inspect.cpp. egress's timer thread
// dedups/encodes and hands the frame to expose (xi.ui.sink), which broadcasts it
// on that channel. The driver subscribes to `ui/cam` and asserts the wire.
//
// This script's only job is the PRODUCT plane, so the driver's cap-absent phase
// can prove mock_camera's product emit is byte-INTACT while the ui_preview push
// is a no-op (no ui_egress loaded): it forwards each frame's raw image + seq to
// expose on channel "cam". (In the live phase this "cam" channel is simply not
// subscribed, so it costs nothing.)
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    auto f = t.pack();
    if (!f) return;                                  // no pack this tick → NA

    int64_t seq = f.get_i64("seq").value_or(-1);
    auto    img = f.get_image("frame");
    if (!img || img->pixels.empty() || seq < 0) { xi::ng(1, "no frame"); return; }

    xi::ScriptPackBuilder b;
    b.add_str("$channel", "cam");
    b.add_i64("$seq", seq);
    b.add_i64("seq", seq);
    b.add_i64("w", img->width);
    b.add_i64("h", img->height);
    b.add_i64("c", img->channels);
    b.add_image("img", img->width, img->height, img->channels, img->pixels.data());
    if (auto out = b.seal()) xi::use("expose").push(out);
    xi::ok(1, "fwd");
}
