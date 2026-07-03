// qa_pack_fault_path — the U1 pack-plane ERROR PATH, live-service, pack-only
// (docs/new_gen/15-pack-fault-semantics.md; parity matrix rows B3/B4; the
// error pattern of io_stress / fixturing_demo: an upstream fault poisons the
// frame, downstream stages never run, and the VERDICT plane carries the
// propagated reason + provenance chain).
//
// Per trigger (mock_camera in PACK MODE drives the pipeline) the script:
//
//   A. PRODUCE — mints a fault pack with xi::ScriptPackBuilder::fault()
//      ("frame_timeout" on key "gray" — the synthetic stand-in for a source
//      door's sensor-drop fault) and stamps the trigger's $seq on it.
//   B. SHORT-CIRCUIT hop 1 — xi::use("det").process(poison): blob_analysis
//      NEVER runs (no blob_count appears); the result is a NEW fault pack
//      carrying the ORIGINAL reason + $seq, with $src="det", $prov="det".
//   C. SHORT-CIRCUIT hop 2 — chaining the propagated fault into "det2"
//      appends the hop: $prov="det/det2", reason still intact.
//   D. PLUGIN-MINTED fault — a REAL door run on a contract-violating input
//      (threshold, no 'gray') faults loud ($fault=missing_input,
//      $fault_key=gray) and is provenance-stamped by the door glue.
//   E. HAPPY CONTROL — a valid square pack still processes normally: no
//      $fault, blob_count==1, and the result carries $src/$prov.
//
// VERDICT: the poisoned frame verdicts NG (xi::ng) with a structured message
// carrying the propagated reason + chain — the driver asserts every frame is
// such an NG with all structural flags green and NO ok verdicts (a poisoned
// frame must never pass).
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>
#include <string>
#include <vector>

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;
    auto tp = t.pack();
    if (!tp) return;                                  // no pack on this tick
    const long long seq = (long long)tp.get_i64("seq").value_or(-1);

    // ---- A. PRODUCE: the script-minted upstream fault ----------------------
    xi::ScriptPackBuilder fb;
    bool built = fb.valid();
    built = fb.fault("frame_timeout", "gray", "qa: synthetic upstream drop") && built;
    built = fb.add_i64("$seq", seq) && built;
    auto poison = fb.seal();
    built = built && poison.valid() && poison.is_fault();

    // ---- B. hop 1: the funnel short-circuits, det never runs ---------------
    auto h1 = xi::use("det").process(poison);
    const bool sc1 = h1.valid() && h1.is_fault()
        && h1.fault_reason().value_or("") == "frame_timeout"
        && h1.fault_key().value_or("") == "gray"
        && !h1.get_i64("blob_count").has_value()      // the door never produced
        && (long long)h1.get_i64("$seq").value_or(-2) == seq
        && h1.src().value_or("") == "det"
        && h1.prov().value_or("") == "det";

    // ---- C. hop 2: the chain accumulates ------------------------------------
    auto h2 = xi::use("det2").process(h1);
    const bool sc2 = h2.valid() && h2.is_fault()
        && h2.fault_reason().value_or("") == "frame_timeout"
        && h2.src().value_or("") == "det2"
        && h2.prov().value_or("") == "det/det2";

    // ---- D. plugin-minted contract fault, provenance-stamped ----------------
    xi::ScriptPackBuilder cb;
    bool cbuilt = cb.valid() && cb.add_i64("threshold", 128);   // no 'gray'
    auto cf = xi::use("det").process(cb.seal());
    const bool mint = cbuilt && cf.valid() && cf.is_fault()
        && cf.fault_reason().value_or("") == "missing_input"
        && cf.fault_key().value_or("") == "gray"
        && cf.src().value_or("") == "det"
        && cf.prov().value_or("") == "det";

    // ---- E. happy control: non-fault packs unaffected -----------------------
    constexpr int W = 16, H = 16;
    std::vector<uint8_t> gray((size_t)W * H, 0);
    for (int y = 5; y < 10; ++y)
        for (int x = 5; x < 10; ++x) gray[(size_t)y * W + x] = 255;
    xi::ScriptPackBuilder hb;
    bool hbuilt = hb.valid();
    hbuilt = hb.add_image("gray", W, H, 1, gray.data()) && hbuilt;
    hbuilt = hb.add_i64("threshold", 128) && hbuilt;
    hbuilt = hb.add_i64("min_area", 1) && hbuilt;
    auto hr = xi::use("det").process(hb.seal());
    const bool happy = hbuilt && hr.valid() && !hr.is_fault()
        && hr.get_i64("blob_count").value_or(-1) == 1
        && hr.src().value_or("") == "det"
        && hr.prov().value_or("") == "det";

    // ---- verdict: NG carrying the propagated reason + chain -----------------
    const std::string reason(h2.fault_reason().value_or("<none>"));
    const std::string prov(h2.prov().value_or("<none>"));
    char msg[224];
    std::snprintf(msg, sizeof msg,
                  "pfault seq=%lld built=%d sc1=%d sc2=%d mint=%d happy=%d "
                  "reason=%s prov=%s",
                  seq, built ? 1 : 0, sc1 ? 1 : 0, sc2 ? 1 : 0, mint ? 1 : 0,
                  happy ? 1 : 0, reason.c_str(), prov.c_str());
    xi::ng(1, msg);   // a poisoned frame NEVER passes — the reason rides the verdict
}
