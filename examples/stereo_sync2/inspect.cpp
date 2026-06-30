//
// inspect.cpp — verify the framework correlates emits from two source
// instances (cam_left, cam_right) into a single trigger event.
//
// Per cycle:
//   - read both images via xi::current_trigger().image("cam_left/right")
//   - decode the embedded LE u32 sequence number from the first 4 bytes
//   - surface active / have_left / have_right / left_seq / right_seq / matched
//     to the `expose` sink on channel "pairs".
//
// VAR was removed from core, so output rides the generic expose sink instead:
// one Record per inspect cycle, pushed via xi::use("expose").process(rec). The
// driver subscribes to "pairs", collects the XEX1 frames, and scores the same
// correlation property the VAR-based test scored.
//
#include <xi/xi.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_use.hpp>

#include <cstdint>
#include <cstring>
#include <string>

static uint32_t decode_seq(const xi::Image& img) {
    if (img.empty() || img.width <= 0 || img.height <= 0) return 0xFFFFFFFFu;
    const uint8_t* px = img.data();
    if (!px) return 0xFFFFFFFFu;
    uint32_t v = 0;
    std::memcpy(&v, px, sizeof(v));   // first 4 bytes = LE u32 seq
    return v;
}

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();

    xi::Record rec;
    if (!t.is_active()) {
        rec.set("active", false);
        rec.set("$channel", "pairs");
        xi::use("expose").process(rec);
        return;
    }

    rec.set("active",    true);
    rec.set("tid",       t.id_string());

    auto left  = t.image("cam_left");
    auto right = t.image("cam_right");

    const bool have_left  = !left.empty();
    const bool have_right = !right.empty();
    rec.set("have_left",  have_left);
    rec.set("have_right", have_right);

    // Report as int (xi::Json doesn't have u32; fits in int64 always
    // because seq = wall_clock_ms / 50 is well within int range for
    // reasonable test durations).
    const long long left_seq  = (long long)decode_seq(left);
    const long long right_seq = (long long)decode_seq(right);
    rec.set("left_seq",  left_seq);
    rec.set("right_seq", right_seq);
    rec.set("matched",   have_left && have_right && left_seq == right_seq);

    rec.set("$channel", "pairs");
    xi::use("expose").process(rec);
}
