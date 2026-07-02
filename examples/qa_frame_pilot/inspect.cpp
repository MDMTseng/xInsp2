// qa_frame_pilot — e2e regression for the wave-2 t.frame() script surface.
//
// mock_camera runs in FRAME MODE, so it emits a v3 Frame on the xi.frame@1 plane;
// the frame rides the dual-carry dispatch to this script, which reads it via
// t.frame() (the borrowed opaque-handle view) and:
//   * verdicts the frame with xi::ok / xi::ng (run_result), and
//   * surfaces seq + image dims on the `expose` channel "qa".
// The driver asserts the sequence advances (t.frame() delivered the "seq" entry)
// and the dims match config — proof the frame reached script hands intact.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    auto f = t.frame();
    if (!f) return;                                  // no frame on this tick → NA

    int64_t seq = f.get_i64("seq").value_or(-1);
    auto    img = f.get_image("frame");
    int w = img ? img->width    : 0;
    int h = img ? img->height   : 0;
    int c = img ? img->channels : 0;

    if (seq >= 0 && img && c == 3 && w > 0 && h > 0) xi::ok(1, "frame ok");
    else                                             xi::ng(1, "malformed frame");

    xi::Record rec;
    rec.set("seq", (int)seq).set("w", w).set("h", h).set("c", c);
    rec.set("$channel", "qa");
    xi::use("expose").process(rec);
}
