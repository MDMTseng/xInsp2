// synced_stereo example — two cameras, ONE trigger. Why that is the whole point.
//
// The naive way to do stereo is two sources and a matching rule: buffer the
// left stream, buffer the right stream, pair frames up by timestamp, hope. Every
// bug that costs you a day lives in that "hope" — a dropped frame shifts the
// pairing by one and nothing downstream can tell.
//
// synced_stereo does not have that problem, because it never creates it. It is
// a GATHERING source: per tick it paints both images and seals them into ONE
// pack, which the host dispatches as ONE trigger. So when this script runs,
// `left` and `right` are not two frames that were matched — they are two named
// entries in a single record that was never apart. A record is a bundle: N
// named images plus values, all of it one atomic thing.
//
//     t.pack()  ->  { seq: 7, left: <320x240x1>, right: <320x240x1> }
//
// This file proves the correlation from the PIXELS rather than trusting the
// container, because "trust the container" is exactly what a reader should not
// take on faith:
//
//   * the plugin memcpy's the tick's `seq` into the first 4 bytes of BOTH
//     images — the direct stamp;
//   * and it paints stripes whose PHASE is derived from seq (left: vertical,
//     `(x+seq)&31`; right: horizontal, `(y+seq)&31`). Recovering that phase
//     from the pixels and finding the same answer on both sides means the two
//     images were painted in the same tick by the same code path. A stale
//     buffer or a one-off pairing error would show up here as a phase skew.
//
// The negative half matters just as much: left and right must be DIFFERENT
// images. "Perfectly correlated" is trivially true if someone hands you the
// same buffer twice, so the script also checks that left really is vertically
// striped (rows identical) and right really is horizontally striped (columns
// identical) — two genuinely distinct views under one trigger id.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>
#include <cstring>

namespace {

constexpr int kPeriod = 32;   // the plugin's stripe period: ((x|y)+seq)&31

// The int32 the plugin memcpy'd into the image's first 4 bytes.
int32_t read_stamp(const uint8_t* p, size_t n) {
    int32_t v = -1;
    if (p && n >= 4) std::memcpy(&v, p, 4);
    return v;
}

// LEFT is vertical stripes: pixel(x,y) = ((x+seq)&31) ? 200 : 32. The single
// DARK column inside the first period sits at x where (x+seq)&31 == 0, i.e. at
// x = (-seq) mod 32. Recovering that x recovers the tick the pixels were painted
// for. Row 1, not row 0 — row 0's first 4 bytes carry the seq stamp.
int left_phase(const uint8_t* p, int w) {
    for (int x = 0; x < kPeriod && x < w; ++x)
        if (p[(size_t)1 * w + x] == 32) return x;
    return -1;
}

// RIGHT is horizontal stripes: pixel(x,y) = ((y+seq)&31) ? 32 : 200. The single
// BRIGHT row inside the first period sits at y = (-seq) mod 32 — the SAME phase
// as left's dark column, by construction. Column 10 avoids the stamp bytes.
int right_phase(const uint8_t* p, int w, int h) {
    for (int y = 0; y < kPeriod && y < h; ++y)
        if (p[(size_t)y * w + 10] == 200) return y;
    return -1;
}

// Orientation checks — the "these are not the same buffer" guard.
bool rows_identical(const uint8_t* p, int w, int h) {          // => vertical stripes
    if (h < 4) return false;
    return std::memcmp(p + (size_t)1 * w, p + (size_t)2 * w, (size_t)w) == 0 &&
           std::memcmp(p + (size_t)2 * w, p + (size_t)3 * w, (size_t)w) == 0;
}
bool cols_identical(const uint8_t* p, int w, int h) {          // => horizontal stripes
    if (w < 4 || h < 8) return false;
    const uint8_t* row = p + (size_t)5 * w;
    for (int x = 1; x < w; ++x) if (row[x] != row[0]) return false;
    return true;
}

}  // namespace

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    // ---- ONE pack, and everything correlated is already inside it ----------
    auto p = t.pack();
    if (!p) return;

    const long long seq = (long long)p.get_i64("seq").value_or(-1);
    auto L = p.image_blob("left");
    auto R = p.image_blob("right");
    if (seq < 0 || !L || !R) {
        // A missing side is not "wait for the partner" — there is no partner to
        // wait for. Either the whole record arrived or none of it did.
        xi::ng(1, "pack is not a stereo record (missing seq/left/right)");
        return;
    }

    const bool dims_ok = L->width == R->width && L->height == R->height &&
                         L->channels == 1 && R->channels == 1 &&
                         L->width == 320 && L->height == 240;

    // ---- correlation proof 1: the seq stamped into both images -------------
    const int32_t sl = read_stamp(L->payload.data(), L->payload.size());
    const int32_t sr = read_stamp(R->payload.data(), R->payload.size());
    const bool stamps_ok = (sl == (int32_t)seq) && (sr == (int32_t)seq);

    // ---- correlation proof 2: the stripe phase, recovered from the pixels ---
    const int expect = (kPeriod - (int)(seq % kPeriod)) % kPeriod;
    const int pl = left_phase(L->payload.data(), L->width);
    const int pr = right_phase(R->payload.data(), R->width, R->height);
    const bool phase_ok = (pl == expect) && (pr == expect);

    // ---- the negative half: two DIFFERENT views, not one buffer twice -------
    const bool vert_l  = rows_identical(L->payload.data(), L->width, L->height);
    const bool horz_r  = cols_identical(R->payload.data(), R->width, R->height);
    const bool differ  = L->payload.size() == R->payload.size() &&
                         std::memcmp(L->payload.data(), R->payload.data(),
                                     L->payload.size()) != 0;

    // ---- show it: both images in ONE exposed record ------------------------
    // The same shape the trigger had. They were bundled when they arrived and
    // they stay bundled on the way out, so the webUI can never show you a left
    // from one tick beside a right from another.
    xi::ScriptPackBuilder e;
    e.add_str("$channel", "stereo");
    e.add_i64("$seq", (int64_t)xi::run_id());
    e.add_i64("seq", seq);
    e.add_i64("stamp_left", sl);
    e.add_i64("stamp_right", sr);
    e.add_i64("phase_left", pl);
    e.add_i64("phase_right", pr);
    e.add_i64("phase_expected", expect);
    e.add_i64("correlated", (stamps_ok && phase_ok) ? 1 : 0);
    e.add_i64("distinct", (vert_l && horz_r && differ) ? 1 : 0);
    e.add_image("left",  L->width, L->height, L->channels, L->payload.data());
    e.add_image("right", R->width, R->height, R->channels, R->payload.data());
    xi::use("expose").push(e.seal());

    char msg[224];
    std::snprintf(msg, sizeof msg,
                  "stereo seq=%lld stamps=%d/%d phase=%d/%d want=%d "
                  "dims=%d vert=%d horz=%d differ=%d",
                  seq, sl, sr, pl, pr, expect,
                  dims_ok ? 1 : 0, vert_l ? 1 : 0, horz_r ? 1 : 0, differ ? 1 : 0);
    if (dims_ok && stamps_ok && phase_ok && vert_l && horz_r && differ)
        xi::ok(1, msg);
    else
        xi::ng(1, msg);
}
