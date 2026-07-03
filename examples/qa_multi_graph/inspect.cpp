// qa_multi_graph — ONE project, ONE script, TWO fully independent inspection
// pipelines (multi-graph topology on dispatch-group lanes).
//
//   line A: camA (mock_camera, pack mode, 64x48 @30fps, group "line_a")
//              -> detA (blob_analysis pack door) -> expose channel "a"
//   line B: camB (mock_camera, pack mode, 96x72 @10fps, group "line_b")
//              -> detB (blob_analysis pack door) -> expose channel "b"
//
// TOPOLOGY: each camera's instance.json carries a `group`; the dispatcher
// stamps every emitted trigger with the EMITTING instance's group
// (service_dispatch.cpp: ev.group = instance_group(ev.leader_source)) and
// routes it to that group's own GroupLane — own queue, own worker thread(s),
// own overflow policy (project.json parallelism.groups). So the two lines
// never share a dispatch queue or a worker: line-lag, drops or faults on one
// lane cannot stall the other. THIS SCRIPT is the only shared text — it runs
// once per trigger, on the trigger's OWN lane, and routes by
// t.primary_source().
//
// Per trigger the script:
//   - line A, normal seq:  builds a deterministic 1-square gray canvas at
//     camA's geometry, runs detA (expect blob_count==1), pushes the facts to
//     expose channel "a" ($seq = camA frame seq), verdicts ok/ng.
//   - line A, seq in [60,120): the DELIBERATE fault window — mints a poison
//     pack via xi::ScriptPackBuilder::fault() (U1's pack-plane fault path,
//     as in qa_pack_fault_path) and feeds it to detA: the funnel
//     short-circuits (the door never runs), the reason + $seq are carried,
//     $prov stamps "detA". Verdict = NG carrying the chain. Channel "a"
//     keeps flowing (fault=1 frames) so its $seq stays monotone through the
//     outage.
//   - line B (every seq): 2-square canvas at camB's geometry through detB
//     (expect blob_count==2), expose channel "b", verdict ok/ng. Line B has
//     NO fault logic at all — the driver proves camA's fault window does not
//     dent line B's cadence (count-based isolation proof).
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// camA frame seqs [kFaultFrom, kFaultTo) are poisoned (2.0s..4.0s at 30fps).
constexpr long long kFaultFrom = 60;
constexpr long long kFaultTo   = 120;

void draw_square(std::vector<uint8_t>& px, int W, int x0, int y0, int s) {
    for (int y = y0; y < y0 + s; ++y)
        for (int x = x0; x < x0 + s; ++x)
            px[(size_t)y * W + x] = 255;
}

// Deterministic analysis input: black canvas at the REAL camera geometry
// (w/h come from the trigger pack's frame — proves the per-line image path)
// with `squares` white 6x6 markers -> blob_count == squares, always.
std::vector<uint8_t> marker_canvas(int w, int h, int squares) {
    std::vector<uint8_t> px((size_t)w * h, 0);
    for (int i = 0; i < squares; ++i)
        draw_square(px, w, 4 + i * 12, 4, 6);
    return px;
}

// One line's normal hop: canvas -> det -> expose channel -> verdict.
void run_line(const char* tag, const char* det, const char* channel,
              long long seq, int w, int h, int c, int expect_blobs) {
    auto px = marker_canvas(w, h, expect_blobs);
    xi::ScriptPackBuilder b;
    bool built = b.valid();
    built = b.add_image("gray", w, h, 1, px.data()) && built;
    built = b.add_i64("threshold", 128) && built;
    built = b.add_i64("min_area", 4) && built;
    built = b.add_i64("$seq", seq) && built;
    auto r = xi::use(det).process(b.seal());

    const long long blobs = r.valid() ? r.get_i64("blob_count").value_or(-1) : -1;
    const std::string prov(r.valid() ? r.prov().value_or("") : "");
    const bool good = built && r.valid() && !r.is_fault()
        && blobs == expect_blobs && prov == det
        && seq >= 0 && c == 3;

    xi::ScriptPackBuilder eb;
    eb.add_str("$channel", channel);
    eb.add_i64("$seq", seq);
    eb.add_i64("seq", seq);
    eb.add_i64("blob", blobs);
    eb.add_i64("w", w);
    eb.add_i64("h", h);
    eb.add_i64("fault", 0);
    if (auto out = eb.seal()) xi::use("expose").push(out);

    char msg[160];
    std::snprintf(msg, sizeof msg, "%s seq=%lld blob=%lld w=%d h=%d c=%d prov=%s",
                  tag, seq, blobs, w, h, c, prov.c_str());
    if (good) xi::ok(1, msg);
    else      xi::ng(1, msg);
}

// Line A's fault-window hop: poison pack -> detA short-circuits -> NG verdict.
void run_line_a_fault(long long seq) {
    xi::ScriptPackBuilder fb;
    bool built = fb.valid();
    built = fb.fault("frame_timeout", "gray", "qa_multi_graph: injected line-A outage") && built;
    built = fb.add_i64("$seq", seq) && built;
    auto poison = fb.seal();
    built = built && poison.valid() && poison.is_fault();

    auto r = xi::use("detA").process(poison);
    const bool sc = built && r.valid() && r.is_fault()
        && r.fault_reason().value_or("") == "frame_timeout"
        && r.fault_key().value_or("") == "gray"
        && !r.get_i64("blob_count").has_value()      // the door never ran
        && (long long)r.get_i64("$seq").value_or(-2) == seq
        && r.prov().value_or("") == "detA";

    // The data plane keeps flowing through the outage: channel "a" $seq stays
    // monotone (fault frames are marked, not dropped).
    xi::ScriptPackBuilder eb;
    eb.add_str("$channel", "a");
    eb.add_i64("$seq", seq);
    eb.add_i64("seq", seq);
    eb.add_i64("fault", 1);
    if (auto out = eb.seal()) xi::use("expose").push(out);

    char msg[160];
    std::snprintf(msg, sizeof msg, "A seq=%lld FAULT sc=%d reason=%s prov=%s",
                  seq, sc ? 1 : 0,
                  std::string(r.valid() ? r.fault_reason().value_or("<none>") : "<invalid>").c_str(),
                  std::string(r.valid() ? r.prov().value_or("<none>") : "<invalid>").c_str());
    xi::ng(1, msg);   // an injected outage never passes
}

}  // namespace

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;                       // synthetic timer tick

    auto tp = t.pack();
    if (!tp) return;                                  // no pack on this tick
    const long long seq = (long long)tp.get_i64("seq").value_or(-1);
    auto img = tp.get_image("frame");
    const int w = img ? img->width    : 0;
    const int h = img ? img->height   : 0;
    const int c = img ? img->channels : 0;

    // Route by trigger source — the graph fork. Each branch already runs on
    // its OWN dispatch lane (camA -> "line_a", camB -> "line_b"). NOTE:
    // t.primary_source() is the router here, not t.has_source() — has_source()
    // keys off the trigger's Record-plane image map, which is EMPTY for a
    // pack-plane emit (the frame rides the pack), so it always answers false
    // on this path.
    const std::string src = t.primary_source();
    if (src == "camA") {
        if (seq >= kFaultFrom && seq < kFaultTo) run_line_a_fault(seq);
        else run_line("A", "detA", "a", seq, w, h, c, /*expect_blobs=*/1);
    } else if (src == "camB") {
        run_line("B", "detB", "b", seq, w, h, c, /*expect_blobs=*/2);
    }
    // any other source: not part of either graph — no verdict.
}
