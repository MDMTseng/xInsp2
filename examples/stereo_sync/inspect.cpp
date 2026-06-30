// inspect.cpp — stereo_sync inspection script.
//
// Reads the active TriggerEvent via xi::current_trigger() and verifies
// that the left/right frames came from the same trigger cycle (identical
// embedded sequence numbers in the first 4 bytes of pixel data).
//
// VAR was removed from core, so the per-cycle pairing evidence is surfaced
// through the `expose` sink on channel "pairs" instead: one Record per inspect
// cycle carrying active / tid / has_left / has_right / left_seq / right_seq /
// matched (and half_trigger when a partial slips through). The driver subscribes
// to "pairs", collects the XEX1 frames, and scores the same pairing property the
// VAR-based test scored.
//
#include <xi/xi.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_use.hpp>

#include <cstring>
#include <cstdint>
#include <string>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    (void)frame;
    auto t = xi::current_trigger();

    xi::Record rec;
    rec.set("active", t.is_active());
    if (!t.is_active()) {
        // No trigger context — happens for one-shot c.run() calls before
        // continuous mode is started, and for the timer-fallback ticks
        // when no source has emitted yet.
        rec.set("$channel", "pairs");
        xi::use("expose").process(rec);
        return;
    }

    rec.set("tid", t.id_string());
    rec.set("timestamp_us", (double)t.timestamp_us());

    auto left  = t.image("cam_left");
    auto right = t.image("cam_right");
    rec.set("has_left",  !left.empty());
    rec.set("has_right", !right.empty());

    if (left.empty() || right.empty()) {
        // Half-trigger — should never happen now that both frames ride the
        // SAME record from the gathering source (no bus policy involved),
        // but log it explicitly so the driver can spot it.
        rec.set("half_trigger", true);
        rec.set("$channel", "pairs");
        xi::use("expose").process(rec);
        return;
    }

    uint32_t seqL = 0, seqR = 0;
    std::memcpy(&seqL, left.data(),  sizeof(uint32_t));
    std::memcpy(&seqR, right.data(), sizeof(uint32_t));
    rec.set("left_seq",  (int)seqL);
    rec.set("right_seq", (int)seqR);
    rec.set("matched", seqL == seqR);

    rec.set("$channel", "pairs");
    xi::use("expose").process(rec);
}
