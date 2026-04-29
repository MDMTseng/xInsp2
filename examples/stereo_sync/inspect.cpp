// inspect.cpp — stereo_sync inspection script.
//
// Reads the active TriggerEvent via xi::current_trigger() and verifies
// that the left/right frames came from the same trigger cycle (identical
// embedded sequence numbers in the first 4 bytes of pixel data).
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
    VAR(active, t.is_active());
    if (!t.is_active()) {
        // No trigger context — happens for one-shot c.run() calls before
        // continuous mode is started, and for the timer-fallback ticks
        // when no source has emitted yet.
        return;
    }

    VAR(tid, t.id_string());
    VAR(timestamp_us, (double)t.timestamp_us());

    auto left  = t.image("cam_left");
    auto right = t.image("cam_right");
    VAR(has_left,  !left.empty());
    VAR(has_right, !right.empty());

    if (left.empty() || right.empty()) {
        // Half-trigger — should never happen under AllRequired policy
        // but log it explicitly so the driver can spot it.
        VAR(half_trigger, true);
        return;
    }

    uint32_t seqL = 0, seqR = 0;
    std::memcpy(&seqL, left.data(),  sizeof(uint32_t));
    std::memcpy(&seqR, right.data(), sizeof(uint32_t));
    VAR(left_seq,  (int)seqL);
    VAR(right_seq, (int)seqR);
    VAR(matched, seqL == seqR);
}
