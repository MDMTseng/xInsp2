// qa_pack_stream — the doc-18 streaming-via-chunking convention, executable
// (docs/new_gen/18-stream-chunking-convention.md; reserved keys
// xi::pack_contract::kStream/kPart/kEof).
//
// Sealed packs are immutable and atomic — no partial visibility. A payload
// produced over time (line-scan strip, clip recording) therefore travels as a
// CONVENTION on top: a sequence of ordinary sealed packs on one lane sharing
// an i64 "$stream" id, each carrying a dense 0-based i64 "$part", the last
// one carrying "$eof"=true, with OVERLAP regions so boundary-crossing
// features stay detectable whole.
//
// Per trigger (mock_camera in PACK MODE drives the pipeline) the script plays
// BOTH sides of the convention over real sealed packs:
//
//   PRODUCER — a 32x64 virtual strip (two 4x4 white features on black) is
//   emitted as 4 overlapping chunk packs via xi::ScriptPackBuilder: stride 16,
//   overlap 4 (>= the feature extent), each chunk carrying $stream/$part/$seq
//   (+$eof on the last), its payload image "strip", and its placement/
//   ownership entries y0/own_h.
//
//   CONSUMER — a doc-18 state machine reassembles within a ONE-CHUNK bounded
//   window: detect whole features per chunk (the overlap rows give a chunk
//   the full view across its trailing boundary), report only features whose
//   anchor (top row) lies in the chunk's exclusive ownership band
//   [y0, y0+own_h) — the exactly-once dedup rule — and enforce the protocol:
//   $part dense on one lane (doc 17 arrival order; a gap is stream_gap, not a
//   reorder buffer), $fault mid-stream poisons the stream (doc 15: abort,
//   discard partials, drop stragglers), missing $eof ends in stream_timeout.
//
// Three streams per run, all QA-asserted on run_result (zero xi::Record):
//   1001 happy    — completes on $eof; feature A (spans the row-16 chunk
//                   boundary — provably invisible whole to any exclusive band,
//                   checked) and feature B (fully inside the shared overlap
//                   band, seen whole by BOTH chunks) each found EXACTLY once;
//                   reassembly window stayed at one chunk.
//   1002 fault    — a deliberately injected mid-stream fault pack
//                   (sensor_drop) aborts the stream with the original reason;
//                   partial results discarded; a straggler chunk after the
//                   poison (even one carrying $eof) is dropped.
//   1003 timeout  — $eof never arrives; the end-of-lane sweep aborts with
//                   stream_timeout.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kW = 32, kH = 64;   // the full virtual strip (never materialized downstream)
constexpr int kStride  = 16;      // exclusive rows owned per chunk
constexpr int kOverlap = 4;       // rows shared with the NEXT chunk (>= kFeat)
constexpr int kFeat    = 4;       // features are 4x4 white squares
constexpr int kParts   = kH / kStride;

// Ground truth. A spans the part0/part1 stride boundary (rows 14..17 across
// row 16): whole only via overlap. B sits entirely inside the shared overlap
// band [16,20): BOTH chunks see it whole — the ownership rule must dedup.
constexpr int kAx = 4,  kAy = 14;
constexpr int kBx = 20, kBy = 16;

std::vector<uint8_t> make_strip() {
    std::vector<uint8_t> px((size_t)kW * kH, 0);
    auto sq = [&](int x, int y) {
        for (int r = 0; r < kFeat; ++r)
            for (int c = 0; c < kFeat; ++c) px[(size_t)(y + r) * kW + (x + c)] = 255;
    };
    sq(kAx, kAy);
    sq(kBx, kBy);
    return px;
}

// Detect WHOLE kFeat x kFeat white squares fully inside a chunk image
// (top-left corner scan); returns (absolute_top_row, x). A square whose rows
// extend past the chunk's coverage is a PARTIAL and is deliberately not found.
std::vector<std::pair<int, int>> detect(const uint8_t* px, int w, int h, int y0) {
    std::vector<std::pair<int, int>> out;
    for (int y = 0; y + kFeat <= h; ++y)
        for (int x = 0; x + kFeat <= w; ++x) {
            if (px[(size_t)y * w + x] != 255) continue;
            if (y > 0 && px[(size_t)(y - 1) * w + x] == 255) continue;  // not the top edge
            if (x > 0 && px[(size_t)y * w + x - 1] == 255) continue;    // not the left edge
            bool whole = true;
            for (int r = 0; r < kFeat && whole; ++r)
                for (int c = 0; c < kFeat && whole; ++c)
                    if (px[(size_t)(y + r) * w + (x + c)] != 255) whole = false;
            if (whole) out.emplace_back(y0 + y, x);
        }
    return out;
}

// ---- PRODUCER: chunk k of the strip as one ordinary sealed pack ------------
xi::ScriptPack make_chunk(long long sid, int part, const std::vector<uint8_t>& strip,
                          long long seq) {
    const int y0 = part * kStride;
    const int h  = std::min(kStride + kOverlap, kH - y0);  // overlap into the next band
    xi::ScriptPackBuilder b;
    b.add_i64("$stream", sid);                 // pack_contract::kStream
    b.add_i64("$part", part);                  // pack_contract::kPart — dense, 0-based
    if (part == kParts - 1) b.add_bool("$eof", true);  // kEof: LAST chunk only
    b.add_i64("$seq", seq);                    // per-chunk arrival correlation (doc 17)
    b.add_i64("y0", y0);                       // placement: absolute first row
    b.add_i64("own_h", kStride);               // exclusive ownership band height
    b.add_image("strip", kW, h, 1, strip.data() + (size_t)y0 * kW);
    return b.seal();
}

// ---- CONSUMER: the doc-18 reassembly state machine over sealed packs -------
struct StreamConsumer {
    long long id = -1;
    long long next_part = 0;
    bool complete = false, aborted = false;
    std::string reason;
    int buffered = 0, max_buffered = 0;              // bounded-window proof
    std::vector<std::pair<int, int>> features;       // exactly-once, absolute coords

    void abort(std::string why) {
        aborted = true; complete = false;
        reason = std::move(why);
        features.clear();                            // doc 15: never consume poisoned partials
    }

    void feed(const xi::ScriptPack& p) {
        if (aborted) return;                         // poisoned stream: stragglers dropped
        if (!p.valid()) { abort("bad_chunk"); return; }
        if (p.is_fault()) {                          // doc 15: $fault poisons the stream
            // F3: doc 15's poison marker is the fault carrying THIS stream's
            // "$stream" id — a fault MUST name the stream it poisons. A fault
            // tagged for ANOTHER stream, or a stream-less fault, is NOT this
            // consumer's poison: ignore it, don't clear the in-progress
            // reassembly (a shared lane carries faults for every stream on it).
            const long long fsid = p.get_i64("$stream").value_or(-1);
            if (id < 0 && fsid >= 0) id = fsid;      // first thing seen binds the stream
            if (fsid >= 0 && fsid == id)
                abort(std::string(p.fault_reason().value_or("fault")));
            return;
        }
        const long long sid  = p.get_i64("$stream").value_or(-1);
        const long long part = p.get_i64("$part").value_or(-1);
        if (id < 0) id = sid;
        if (sid != id)         { abort("stream_mismatch"); return; }
        if (complete)          { abort("part_after_eof");  return; }
        if (part != next_part) { abort("stream_gap");      return; }  // doc 17: no reorder on one lane

        auto img = p.get_image("strip");
        const long long y0  = p.get_i64("y0").value_or(-1);
        const long long own = p.get_i64("own_h").value_or(-1);
        if (!img || img->channels != 1 || y0 < 0 || own <= 0) { abort("bad_chunk"); return; }

        ++buffered;                                  // the window: this chunk, and only it
        max_buffered = std::max(max_buffered, buffered);
        for (const auto& f : detect(img->pixels.data(), img->width, img->height, (int)y0))
            if (f.first >= y0 && f.first < y0 + own) // ownership band: exactly-once dedup
                features.push_back(f);
        --buffered;                                  // window closes: chunk released
        ++next_part;
        if (p.get_bool("$eof").value_or(false)) complete = true;
    }

    void finish() {                                  // end of lane, $eof never arrived
        if (!complete && !aborted) abort("stream_timeout");
    }
};

} // namespace

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;
    auto tp = t.pack();
    if (!tp) return;                                 // no pack on this tick → NA
    const long long seq = (long long)tp.get_i64("seq").value_or(-1);
    const auto strip = make_strip();

    // ---- stream 1001: happy path — M overlapping chunks, $eof on the last --
    StreamConsumer s1;
    bool sealed_ok = true;
    for (int k = 0; k < kParts; ++k) {
        auto c = make_chunk(1001, k, strip, seq);
        sealed_ok = sealed_ok && c.valid();
        s1.feed(c);
    }
    s1.finish();
    int na = 0, nb = 0, nother = 0;
    for (const auto& f : s1.features) {
        if (f.first == kAy && f.second == kAx)      ++na;
        else if (f.first == kBy && f.second == kBx) ++nb;
        else                                        ++nother;
    }
    const bool s1_ok = s1.complete && !s1.aborted &&
                       na == 1 && nb == 1 && nother == 0 &&   // exactly once each
                       s1.max_buffered == 1;                  // one-chunk window held

    // OVERLAP NECESSITY: A straddles the stride boundary, so exclusive bands
    // alone (a zero-overlap chunking) never see it whole — the boundary
    // feature is provable ONLY via the overlap rows.
    int a_whole_without_overlap = 0;
    for (int k = 0; k < kParts; ++k) {
        const int y0 = k * kStride;
        for (const auto& f : detect(strip.data() + (size_t)y0 * kW, kW, kStride, y0))
            if (f.first == kAy && f.second == kAx) ++a_whole_without_overlap;
    }
    const bool overlap_needed = a_whole_without_overlap == 0 &&
                                kAy < kStride && kAy + kFeat > kStride;

    // ---- stream 1002: deliberately injected mid-stream fault ---------------
    StreamConsumer s2;
    s2.feed(make_chunk(1002, 0, strip, seq));
    s2.feed(make_chunk(1002, 1, strip, seq));
    xi::ScriptPackBuilder fb;                        // the poison, in part 2's slot
    fb.fault("sensor_drop", "strip", "qa: injected mid-stream drop");
    fb.add_i64("$stream", 1002);
    fb.add_i64("$part", 2);
    fb.add_i64("$seq", seq);
    s2.feed(fb.seal());
    s2.feed(make_chunk(1002, 3, strip, seq));        // straggler (carries $eof): dropped
    s2.finish();
    const bool s2_ok = s2.aborted && !s2.complete &&
                       s2.reason == "sensor_drop" &&          // the ORIGINAL reason
                       s2.features.empty();                   // partials discarded

    // ---- stream 1003: $eof never arrives → consumer-owned timeout ----------
    StreamConsumer s3;
    s3.feed(make_chunk(1003, 0, strip, seq));
    s3.feed(make_chunk(1003, 1, strip, seq));
    s3.finish();                                     // the lane's deadline expires
    const bool s3_ok = s3.aborted && !s3.complete && s3.reason == "stream_timeout";

    // ---- stream 1004: a FOREIGN fault must NOT poison this stream (F3) -------
    // A shared lane carries faults for every stream on it. A fault tagged for
    // ANOTHER stream (1002), and a stream-less fault, are delivered mid-flight to
    // consumer 1004: neither may clear its reassembly. The stream still completes
    // on its own $eof with both features found exactly once.
    StreamConsumer s4;
    s4.feed(make_chunk(1004, 0, strip, seq));        // binds id = 1004
    s4.feed(make_chunk(1004, 1, strip, seq));
    {
        xi::ScriptPackBuilder foreign;               // a fault for a DIFFERENT stream
        foreign.fault("sensor_drop", "strip", "qa: fault for stream 1002");
        foreign.add_i64("$stream", 1002);
        foreign.add_i64("$part", 2);
        foreign.add_i64("$seq", seq);
        s4.feed(foreign.seal());
        xi::ScriptPackBuilder streamless;            // a fault with NO stream id
        streamless.fault("global_glitch", "strip", "qa: stream-less fault");
        streamless.add_i64("$seq", seq);
        s4.feed(streamless.seal());
    }
    s4.feed(make_chunk(1004, 2, strip, seq));        // reassembly must be intact
    s4.feed(make_chunk(1004, 3, strip, seq));        // carries $eof
    s4.finish();
    int s4a = 0, s4b = 0, s4other = 0;
    for (const auto& f : s4.features) {
        if (f.first == kAy && f.second == kAx)      ++s4a;
        else if (f.first == kBy && f.second == kBx) ++s4b;
        else                                        ++s4other;
    }
    const bool s4_ok = s4.complete && !s4.aborted &&     // the foreign faults were ignored
                       s4a == 1 && s4b == 1 && s4other == 0;

    const bool pass = seq >= 0 && sealed_ok && s1_ok && overlap_needed &&
                      s2_ok && s3_ok && s4_ok;
    char msg[256];
    std::snprintf(msg, sizeof msg,
                  "pstream seq=%lld sealed=%d s1(complete=%d a=%d b=%d other=%d win=%d) "
                  "overlap_needed=%d s2(%d:%s) s3(%d:%s) s4(%d:a=%d b=%d)",
                  seq, sealed_ok ? 1 : 0, s1.complete ? 1 : 0, na, nb, nother,
                  s1.max_buffered, overlap_needed ? 1 : 0, s2_ok ? 1 : 0,
                  s2.reason.c_str(), s3_ok ? 1 : 0, s3.reason.c_str(),
                  s4_ok ? 1 : 0, s4a, s4b);
    if (pass) xi::ok(1, msg);                        // the verdict rides run_result
    else      xi::ng(1, msg);

    // Surface the structural flags for the driver on the expose channel.
    xi::ScriptPackBuilder rb;
    rb.add_str("$channel", "stream");
    rb.add_i64("$seq", seq);
    rb.add_i64("seq", seq);
    rb.add_i64("feat_a", na);
    rb.add_i64("feat_b", nb);
    rb.add_i64("feat_other", nother);
    rb.add_i64("s1_complete", s1.complete ? 1 : 0);
    rb.add_i64("win", s1.max_buffered);
    rb.add_i64("overlap_needed", overlap_needed ? 1 : 0);
    rb.add_i64("s2_fault_abort", s2_ok ? 1 : 0);
    rb.add_i64("s3_timeout", s3_ok ? 1 : 0);
    rb.add_i64("s4_foreign_fault_ignored", s4_ok ? 1 : 0);
    if (auto out = rb.seal()) xi::use("expose").push(out);
}
