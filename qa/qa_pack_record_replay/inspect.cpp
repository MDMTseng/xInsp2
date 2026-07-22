// qa_pack_record_replay — the graph-level RECORD -> REPLAY composing example
// (docs/new_gen/12-pack-parity-matrix.md rows E1/E2/E3; the example owed by the
// doc 10 Gate P2 verdict). The full persistence loop AS A PROJECT, pack-only:
//
//   phase 1 (RECORD): mock_camera in PACK MODE drives the graph. Per camera
//     trigger the script lifts the frame, rebuilds it as a routed capture pack
//     ($channel/$seq routing entries + a psum pixel checksum + a nested
//     canonical-mp "meta" entry + the image), and routes it into record_save's
//     xi.pack@1 door with xi::use("rec").process(cap) — one canonical XEX1-v3
//     file lands in the rec instance's captures folder per trigger (the ack
//     pack is asserted). The SAME sealed pack is also use("expose").push()ed
//     on channel "rec" so the driver holds the recorded truth on the wire.
//
//   pump: the synthetic timer tick (start fps>0 => t.is_active()==false)
//     drives the replay source: one xi::use("replay").process(empty pack) per
//     tick replays the NEXT .xex1 file (record_replay README "driving model").
//     While the instance is disabled (phase 1) this is an honest no-op ack —
//     the DRIVER phases the run by flipping the instance's `enabled` config,
//     the script never changes shape.
//
//   phase 2 (REPLAY): each replayed file re-enters the graph as a fresh
//     sealed-pack trigger (t.primary_source()=="replay") — the A1 source-emit
//     path, same as the camera. The script verifies the restored reserved
//     entries ($channel=="rec", $seq==seq), recomputes the pixel checksum
//     against the recorded psum entry, checks the nested mp entry survived,
//     counts entries (6), and re-surfaces the replayed content on wire
//     channel "rep" so the driver can compare disk bytes vs recorded wire vs
//     replayed wire, entry-for-entry and pixel-byte-for-byte.
//
// Zero xi::Record data-plane use: capture, route, replay, verify — all packs.
// (The empty pack is the replay source's "advance one file" verb.)
// The byte-level parity oracle for the loop is toolbox/record_replay_pack_test.cpp;
// THIS example is the composition proof that the loop is expressible as a
// real project (source -> script routing -> sink door -> replay source -> wire).
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>
#include <string>

namespace {
long long pixel_sum(const xi::ImageBlobView& img) {
    long long s = 0;
    for (uint8_t b : img.payload) s += b;
    return s;
}
}  // namespace

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;

    if (!t.is_active()) {
        // Synthetic timer tick: pump the replay source — one process() call =
        // one file re-emitted into the graph (disabled => honest no-op ack).
        // A one-entry "advance" pack is the source's "next file" verb (an EMPTY
        // pack would short-circuit host-side and never reach the door).
        xi::ScriptPackBuilder pump;
        pump.add_i64("advance", 1);
        xi::use("replay").process(pump.seal());
        return;
    }

    auto tp = t.pack();
    if (!tp) return;                       // Record-era trigger: not this graph
    const std::string src = t.primary_source();

    if (src == "cam") {
        // ---- phase 1: RECORD — route the camera pack into record_save ------
        const long long seq = (long long)tp.get_i64("seq").value_or(-1);
        auto img = tp.image_blob("frame");
        if (!img) { xi::ng(1, "rec: camera pack has no 'frame' image"); return; }
        const long long psum = pixel_sum(*img);

        xi::mp::Writer meta;               // nested canonical entry, rides the file
        meta.map(2);
        meta.key("origin");      meta.str("cam");
        meta.key("trigger_seq"); meta.int_(seq);

        xi::ScriptPackBuilder b;
        bool built = b.valid();
        built = b.add_str("$channel", "rec") && built;   // file header channel
        built = b.add_i64("$seq", seq) && built;         // file header seq
        built = b.add_i64("seq", seq) && built;
        built = b.add_i64("psum", psum) && built;        // self-validating checksum
        built = b.add_mp("meta", meta) && built;
        built = b.add_image_blob("frame", img->width, img->height, img->channels, "u8",
                            img->payload.data(),
                            (int64_t)img->width * img->height * img->channels) && built;
        auto cap = b.seal();
        if (!(built && cap.valid())) { xi::ng(1, "rec: capture pack build failed"); return; }

        // The pack door: record_save persists cap as ONE canonical .xex1 and
        // acks. Then the SAME sealed pack goes to the wire (channel "rec") —
        // one handle, two consumers, zero copies.
        auto ack = xi::use("rec").process(cap);
        const bool     saved  = ack.get_bool("saved").value_or(false);
        const bool     fault  = ack.get_str("$fault").has_value();
        const long long bytes = (long long)ack.get_i64("bytes").value_or(-1);
        const bool     named  = ack.get_str("base_name").has_value();
        const bool     pushed = xi::use("expose").push(cap);

        const bool pass = seq >= 0 && saved && !fault && bytes > 0 && named && pushed;
        char msg[192];
        std::snprintf(msg, sizeof msg,
                      "rec seq=%lld psum=%lld saved=%d fault=%d bytes=%lld pushed=%d",
                      seq, psum, saved ? 1 : 0, fault ? 1 : 0, bytes, pushed ? 1 : 0);
        if (pass) xi::ok(1, msg);
        else      xi::ng(1, msg);
        return;
    }

    if (src == "replay") {
        // ---- phase 2: REPLAY — verify the re-emitted pack, re-surface it ---
        const bool fault = tp.get_str("$fault").has_value();
        if (fault) {                        // corrupt file => sealed $fault pack
            xi::ng(1, "rep: $fault pack from replay (bad file on disk)");
            return;
        }
        const bool  chan_ok = tp.get_str("$channel").value_or("") == "rec";
        const long long rseq = (long long)tp.get_i64("$seq").value_or(-1);
        const long long seq  = (long long)tp.get_i64("seq").value_or(-2);
        const long long psum = (long long)tp.get_i64("psum").value_or(-1);
        auto meta = tp.get_mp("meta");
        auto img  = tp.image_blob("frame");

        int entries = 0;
        tp.for_each([&](auto&&, auto&&) { ++entries; });

        const bool seq_ok  = rseq >= 0 && seq == rseq;       // $seq restored == seq entry
        const bool sum_ok  = img && psum >= 0 && pixel_sum(*img) == psum;
        const bool meta_ok = meta && !meta->empty();

        // Re-surface the replayed content on its own wire channel: the driver
        // compares disk file vs "rec" wire vs THIS, entry- and byte-level.
        xi::ScriptPackBuilder rb;
        bool rebuilt = rb.valid();
        rebuilt = rb.add_str("$channel", "rep") && rebuilt;
        rebuilt = rb.add_i64("$seq", rseq) && rebuilt;
        rebuilt = rb.add_i64("seq", seq) && rebuilt;
        rebuilt = rb.add_i64("psum", psum) && rebuilt;
        if (meta) rebuilt = rb.add_mp("meta", *meta) && rebuilt;
        if (img)  rebuilt = rb.add_image_blob("frame", img->width, img->height,
                                         img->channels, "u8", img->payload.data(),
                                         (int64_t)img->width * img->height * img->channels) && rebuilt;
        auto rep = rb.seal();
        const bool pushed = rebuilt && rep.valid() && xi::use("expose").push(rep);

        const bool pass = chan_ok && seq_ok && sum_ok && meta_ok &&
                          entries == 6 && img && pushed;
        char msg[224];
        std::snprintf(msg, sizeof msg,
                      "rep seq=%lld chan=%d seqok=%d psum=%d meta=%d entries=%d "
                      "img=%d pushed=%d",
                      seq, chan_ok ? 1 : 0, seq_ok ? 1 : 0, sum_ok ? 1 : 0,
                      meta_ok ? 1 : 0, entries, img ? 1 : 0, pushed ? 1 : 0);
        if (pass) xi::ok(1, msg);
        else      xi::ng(1, msg);
        return;
    }
}
