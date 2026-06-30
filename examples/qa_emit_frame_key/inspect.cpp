// qa_emit_frame_key — BUG #19 regression e2e.
//
// The "cam" source (local_image_source) emits a SINGLE image as
// Record().image("frame", img). This script reads it two ways and reports the
// width each read yields via xi::result() (VAR/EMIT are no-ops in this core, so
// the run_result event is the wire channel):
//   fw : t.image("frame")           — the documented contract (must be non-zero
//                                      from a LIVE source AND via cmd:run inject)
//   nw : t.image(<instance name>)   — legacy instance-name read; the reader-side
//                                      sole-image fallback must still resolve it
// Verdict: ok(1,"fw=W nw=W") when BOTH reads got the frame; ng(2,...) otherwise.
// Before the fix a live source stored the lone frame under the INSTANCE NAME, so
// fw was 0 from the live path (yet non-zero via cmd:run) — the silent contradiction.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>
#include <cstdio>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    if (!t.is_active()) return;                 // skip synthetic timer ticks

    xi::Image byframe = t.image("frame");             // documented key
    xi::Image byname  = t.image(t.primary_source());  // legacy instance-name read
    int fw = byframe.empty() ? 0 : (int)byframe.width;
    int nw = byname.empty()  ? 0 : (int)byname.width;

    char msg[64];
    std::snprintf(msg, sizeof(msg), "fw=%d nw=%d", fw, nw);
    if (fw > 0 && nw > 0) xi::ok(1, msg);
    else                  xi::ng(2, msg);
}
