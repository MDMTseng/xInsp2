// qa_pack_stereo — PACK-ONLY stereo gathering in the live service (Gate P2 row:
// "multi-image gathered trigger").
//
// synced_stereo runs in PACK MODE: each tick it gathers the correlated
// left+right pair plus the shared `seq` counter into ONE sealed xi.pack@1 Pack
// and emits it under a single trigger — no bus policy, no Record. This script
// reads the pack via t.pack() and verdicts it; NOTHING here touches xi::Record
// (observability rides the run_result verdict plane alone), so the example is
// pack-only end to end — stricter than qa_pack_pilot, whose expose leg still
// re-surfaces values through a Record (the expose-from-script gap).
//
// Checks per pack:
//   * `seq` (i64), `left` and `right` (image 320x240x1) are all present;
//   * dims match the plugin's fixed geometry;
//   * the seq stamped into the first 4 bytes of BOTH images equals the pack's
//     `seq` entry — the "these two frames really came from the same event"
//     correlation proof, now asserted in script hands in the running backend
//     (previously proven only host-side in test_synced_stereo.cpp).
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>
#include <cstdio>
#include <cstring>

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    auto f = t.pack();
    if (!f) return;                                  // no pack on this tick → NA

    int64_t seq = f.get_i64("seq").value_or(-1);
    auto L = f.image_blob("left");
    auto R = f.image_blob("right");
    if (seq < 0 || !L || !R) {
        xi::ng(1, "pack missing seq/left/right");
        return;
    }

    bool dims_ok = L->width == 320 && L->height == 240 && L->channels == 1 &&
                   R->width == 320 && R->height == 240 && R->channels == 1;

    // The plugin memcpy's the native int32 seq into each image's first 4 bytes.
    int32_t lseq = -1, rseq = -1;
    if (L->payload.size() >= 4) std::memcpy(&lseq, L->payload.data(), 4);
    if (R->payload.size() >= 4) std::memcpy(&rseq, R->payload.data(), 4);
    bool corr_ok = (lseq == (int32_t)seq) && (rseq == (int32_t)seq);

    char msg[128];
    std::snprintf(msg, sizeof msg, "stereo seq=%lld lseq=%d rseq=%d dims=%d",
                  (long long)seq, lseq, rseq, dims_ok ? 1 : 0);
    if (dims_ok && corr_ok) xi::ok(1, msg);
    else                    xi::ng(1, msg);
}
