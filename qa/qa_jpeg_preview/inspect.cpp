// qa_jpeg_preview — the E2 driver script. expose resolves the xi.jpeg.encode
// capability (provided by the `codec` imgcodec instance, when present) and, on
// its v3 WS-SEND path, carries a FULL-RESOLUTION compressed `preview` entry
// instead of raw pixels. This script is a pure PACK producer: each synthetic
// tick it builds fixed images and PUSHES them to the expose sink; the Python
// driver subscribes, collects the binary XEX1 frames, and asserts the wire.
//
// Per tick it pushes THREE channels:
//   "a", "b" — the SAME fixed 128x96 gray image (identical bytes) on two
//              channels. imgcodec dedups by content, so ONE encode serves both
//              channels across every tick (the dedup headline: encodes == 1).
//   "bad"    — a 2-channel image imgcodec REFUSES (contract $fault "wrong_type").
//              expose must fail OPEN: that entry falls back to RAW px while the
//              good channels keep previewing (per-image fail-open).
//
// The SAME script runs against the no_codec project (expose, no imgcodec): there
// the capability is absent, expose runs its persistent degraded/raw path, and
// every image entry rides raw — nothing breaks.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// A fixed, deterministic, compressible 128x96 gray image (smooth gradient + a
// bright square). Identical bytes every call, so imgcodec's content memo dedups
// across channels and ticks; big enough that the JPEG is << the raw plane.
constexpr int W = 128, H = 96;

const std::vector<uint8_t>& fixed_gray() {
    static const std::vector<uint8_t> px = [] {
        std::vector<uint8_t> v((size_t)W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                v[(size_t)y * W + x] = (uint8_t)((x * 2 + y) & 0xFF);
        for (int y = 30; y < 60; ++y)
            for (int x = 40; x < 90; ++x) v[(size_t)y * W + x] = 250;
        return v;
    }();
    return px;
}

bool push_image(const char* channel, long long seq, int w, int h, int c,
                const uint8_t* px) {
    xi::ScriptPackBuilder b;
    bool ok = b.valid();
    ok = b.add_str("$channel", channel) && ok;
    ok = b.add_i64("$seq", seq) && ok;
    ok = b.add_image("img", w, h, c, px) && ok;
    auto pack = b.seal();
    if (!(ok && pack.valid())) return false;
    return xi::use("expose").push(pack);
}

}  // namespace

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (t.is_active()) return;             // only drive on the synthetic timer tick

    static long long seq = 0;
    const long long s = seq++;

    const auto& gray = fixed_gray();
    const bool pa = push_image("a",   s, W, H, 1, gray.data());
    const bool pb = push_image("b",   s, W, H, 1, gray.data());

    // 2-channel image: imgcodec refuses (1/3/4 only) -> contract $fault ->
    // expose falls this entry open to RAW while a/b keep previewing.
    static const std::vector<uint8_t> bad((size_t)8 * 8 * 2, 128);
    const bool pbad = push_image("bad", s, 8, 8, 2, bad.data());

    char msg[128];
    std::snprintf(msg, sizeof msg, "preview seq=%lld a=%d b=%d bad=%d",
                  s, pa ? 1 : 0, pb ? 1 : 0, pbad ? 1 : 0);
    if (pa && pb && pbad) xi::ok(1, msg);
    else                  xi::ng(1, msg);
}
