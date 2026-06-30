// qa_emit_frame_key — BUG #19 regression e2e.
//
// The "cam" source (local_image_source) emits a SINGLE image as
// Record().image("frame", img). This script reads it two ways and surfaces the
// result through the `expose` plugin on channel "qa" (VAR/EMIT are no-ops in
// this core; expose is the official data-out surface). Per frame it pushes:
//   values:  fw = width of t.image("frame")            — the documented contract
//                 (must be non-zero from a LIVE source AND via cmd:run inject)
//            nw = width of t.image(<instance name>)     — legacy instance-name
//                 read; the reader-side sole-image fallback must still resolve it
//   image:   the frame read via image("frame"), RE-EMITTED keyed "frame" — the
//            consumer asserts it arrives keyed "frame" (not the instance name).
// Before the fix a live source stored the lone frame under the INSTANCE NAME, so
// fw was 0 from the live path (yet non-zero via cmd:run) — the silent
// contradiction; AND nothing keyed "frame" would reach the consumer.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    if (!t.is_active()) return;                 // skip synthetic timer ticks

    xi::Image byframe = t.image("frame");             // documented key
    xi::Image byname  = t.image(t.primary_source());  // legacy instance-name read
    int fw = byframe.empty() ? 0 : (int)byframe.width;
    int nw = byname.empty()  ? 0 : (int)byname.width;

    xi::Record rec;
    rec.set("fw", fw).set("nw", nw);
    if (!byframe.empty()) rec.image("frame", byframe);   // re-emit keyed "frame"
    rec.set("$channel", "qa");
    xi::use("expose").process(rec);
}
