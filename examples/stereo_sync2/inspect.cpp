//
// inspect.cpp — verify the framework correlates emits from two source
// instances (cam_left, cam_right) into a single trigger event.
//
// Per cycle:
//   - read both images via xi::current_trigger().image("cam_left/right")
//   - decode the embedded LE u32 sequence number from the first 4 bytes
//   - report VAR(left_seq), VAR(right_seq), VAR(matched).
//
#include <xi/xi.hpp>
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
    if (!t.is_active()) {
        VAR(active, false);
        return;
    }

    VAR(active,    true);
    VAR(tid,       t.id_string());

    auto left  = t.image("cam_left");
    auto right = t.image("cam_right");

    VAR(have_left,  !left.empty());
    VAR(have_right, !right.empty());

    // Report as int (xi::Json doesn't have u32; fits in int64 always
    // because seq = wall_clock_ms / 50 is well within int range for
    // reasonable test durations).
    VAR(left_seq,  (long long)decode_seq(left));
    VAR(right_seq, (long long)decode_seq(right));
    VAR(matched,   have_left && have_right && left_seq == right_seq);
}
