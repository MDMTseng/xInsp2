// qa_pack_pilot — e2e regression for the wave-2 t.pack() script surface.
//
// mock_camera runs in PACK MODE, so it emits a v3 Pack on the xi.pack@1 plane;
// the pack rides the dual-carry dispatch to this script, which reads it via
// t.pack() (the borrowed opaque-handle view) and:
//   * verdicts the pack with xi::ok / xi::ng (run_result), and
//   * surfaces seq + image dims on the `expose` channel "qa".
// The driver asserts the sequence advances (t.pack() delivered the "seq" entry)
// and the dims match config — proof the pack reached script hands intact.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    auto f = t.pack();
    if (!f) return;                                  // no pack on this tick → NA

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
