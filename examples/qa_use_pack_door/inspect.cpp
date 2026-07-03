// qa_use_pack_door — THE Gate P2 flagship: the full pack WRITE-half loop in
// ONE script, live-service, pack-only (docs/new_gen/12-pack-parity-matrix.md
// rows B1/B2, C1, D1/D2-U4; docs/new_gen/10 gate P2).
//
// Per trigger (mock_camera in PACK MODE drives the pipeline) the script:
//
//   1. SCRIPT-BUILD (D1) — assembles a door input with xi::ScriptPackBuilder:
//      a 16x16 gray image with a 5x5 white square (the established
//      blob-analysis door pattern, plugins/use_pack_door_test.cpp) + threshold/
//      min_area scalars + a NESTED canonical-mp entry written with
//      xi::mp::Writer (U4). The sealed pack's mp entry must read back
//      byte-identical to the Writer's bytes (canonicalize is the identity on
//      canonical input).
//   2. USE-DOOR CHAIN (B1/B2) — xi::use("det").process(pack) drives
//      blob_analysis's xi.pack@1 door and asserts on the result entries:
//      blob_count==1, threshold_used==128, the binary image rides back.
//   3. EXPOSE-FROM-SCRIPT (C1, C2, D2) — builds a RESULT pack (verdict
//      scalars + the derived binary image + the nested mp entry + $channel/
//      $seq routing entries) and xi::use("expose").push()es it; a SECOND pack
//      (the synthetic input image) goes to channel "qa2" (multi-channel).
//      The driver decodes the XEX1-v3 wire frames, byte-checks the pushed
//      pixels on both channels, and decodes the nested mp entry.
//
// No xi::Record anywhere: input built script-side, chained script-side,
// surfaced script-side — the loop the parity matrix calls the write half.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <algorithm>
#include <cstdio>
#include <vector>

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    auto tp = t.pack();
    if (!tp) return;                                 // no pack on this tick → NA
    const long long seq = (long long)tp.get_i64("seq").value_or(-1);

    // ---- 1. SCRIPT-BUILD: the door input, assembled in script hands --------
    constexpr int W = 16, H = 16;
    std::vector<uint8_t> gray((size_t)W * H, 0);
    for (int y = 5; y < 10; ++y)
        for (int x = 5; x < 10; ++x) gray[(size_t)y * W + x] = 255;

    xi::mp::Writer meta;                             // nested canonical value (U4)
    meta.map(2);
    meta.key("origin");      meta.str("script");
    meta.key("trigger_seq"); meta.int_(seq);

    xi::ScriptPackBuilder b;
    bool built = b.valid();
    built = b.add_image("gray", W, H, 1, gray.data()) && built;
    built = b.add_i64("threshold", 128) && built;
    built = b.add_i64("min_area", 1) && built;
    built = b.add_mp("meta", meta) && built;
    auto in = b.seal();
    built = built && in.valid();

    // U4 round-trip: the canonical Writer bytes are stored byte-identical.
    bool mp_ok = false;
    if (auto stored = in.get_mp("meta"))
        mp_ok = stored->size() == meta.bytes().size() &&
                std::equal(stored->begin(), stored->end(), meta.bytes().begin());

    // ---- 2. USE-DOOR: chain the script-built pack into blob_analysis -------
    auto out = xi::use("det").process(in);
    const long long blobs = (long long)out.get_i64("blob_count").value_or(-1);
    const long long thr   = (long long)out.get_i64("threshold_used").value_or(-1);
    auto bin = out.get_image("binary");
    const bool fault   = out.get_str("$fault").has_value();
    const bool door_ok = out.valid() && !fault && blobs == 1 && thr == 128 &&
                         bin && bin->width == W && bin->height == H &&
                         bin->channels == 1;

    // ---- 3. EXPOSE-FROM-SCRIPT: push a result pack to the sink -------------
    xi::ScriptPackBuilder rb;
    bool rbuilt = rb.valid();
    rbuilt = rb.add_str("$channel", "qa") && rbuilt;   // routing: expose channel
    rbuilt = rb.add_i64("$seq", seq) && rbuilt;        // ordering: wire seq
    rbuilt = rb.add_i64("seq", seq) && rbuilt;
    rbuilt = rb.add_i64("blob_count", blobs) && rbuilt;
    rbuilt = rb.add_mp("meta", meta) && rbuilt;        // nested results on the wire (D2)
    if (bin)
        rbuilt = rb.add_image("binary", bin->width, bin->height, bin->channels,
                              bin->pixels.data()) && rbuilt;
    auto result = rb.seal();
    const bool pushed = rbuilt && result.valid() && xi::use("expose").push(result);

    // Multi-channel (C2): the script-built synthetic image on its own channel.
    xi::ScriptPackBuilder pb;
    bool pbuilt = pb.valid();
    pbuilt = pb.add_str("$channel", "qa2") && pbuilt;
    pbuilt = pb.add_i64("$seq", seq) && pbuilt;
    pbuilt = pb.add_i64("seq", seq) && pbuilt;
    pbuilt = pb.add_image("gray", W, H, 1, gray.data()) && pbuilt;
    auto preview = pb.seal();
    const bool pushed2 = pbuilt && preview.valid() && xi::use("expose").push(preview);

    const bool pass = seq >= 0 && built && mp_ok && door_ok && pushed && pushed2;
    char msg[192];
    std::snprintf(msg, sizeof msg,
                  "pdoor seq=%lld built=%d mp=%d blobs=%lld thr=%lld bin=%d "
                  "fault=%d pushed=%d pushed2=%d",
                  seq, built ? 1 : 0, mp_ok ? 1 : 0, blobs, thr, bin ? 1 : 0,
                  fault ? 1 : 0, pushed ? 1 : 0, pushed2 ? 1 : 0);
    if (pass) xi::ok(1, msg);
    else      xi::ng(1, msg);
}
