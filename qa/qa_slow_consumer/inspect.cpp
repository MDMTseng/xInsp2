// qa_slow_consumer — the RT8 / doc 21 §P2 lane-liveness driver script (see
// docs/new_gen/23-rt8-async-writer.md). A pure PACK producer: each synthetic
// tick it pushes a modest RAW preview on two channels to the `expose` sink. The
// Python driver subscribes with a deliberately SLOW WS reader and asserts the
// backend keeps computing frames at ~full rate (not pinned to the slow drain),
// that received frames are in strict per-channel seq order, and that a fully-
// wedged client is dropped cleanly while the lane keeps serving a fresh client.
//
// Modest previews (128x96x1 raw ≈ 12 KiB/channel) keep the byte volume high
// enough that a slow drain would visibly pin a SYNCHRONOUS send, yet low enough
// that the post-fix async queue stays well under the 64 MiB backpressure cap over
// the measurement window.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdint>
#include <vector>

namespace {

constexpr int W = 128, H = 96;

// A cheap, deterministic gray image whose content changes per tick (so the wire
// is not trivially dedup-collapsed): a gradient offset by the sequence number.
std::vector<uint8_t> make_gray(long long s) {
    std::vector<uint8_t> v((size_t)W * H);
    const uint8_t off = (uint8_t)(s & 0xFF);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            v[(size_t)y * W + x] = (uint8_t)((x + y + off) & 0xFF);
    return v;
}

bool push_image(const char* channel, long long seq, const uint8_t* px) {
    xi::ScriptPackBuilder b;
    bool ok = b.valid();
    ok = b.add_str("$channel", channel) && ok;
    ok = b.add_i64("$seq", seq) && ok;
    ok = b.add_image("img", W, H, 1, px) && ok;
    auto pack = b.seal();
    if (!(ok && pack.valid())) return false;
    return xi::use("expose").push(pack);
}

}  // namespace

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (t.is_active()) return;   // drive only on the synthetic timer tick

    static long long seq = 0;
    const long long s = seq++;

    const auto gray = make_gray(s);
    const bool pa = push_image("a", s, gray.data());
    const bool pb = push_image("b", s, gray.data());

    if (pa && pb) xi::ok(1, "preview pushed");
    else          xi::ng(1, "preview push failed");
}
