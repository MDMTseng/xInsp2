//
// inspect.cpp — qa_pack_order (U3, docs/new_gen/17): qa_result_order's
// arrival-ordered pattern expressed PACK-ONLY. Uneven inspect time (every 5th
// frame is slow) so that under dispatch_threads>1 a later, faster frame
// finishes before an earlier slow one; with result_order:"arrival" the staged
// sink flush must still deliver in frame-arrival order.
//
// The Record-era version relied on the HOST stamping $seq (= run_id) into the
// staged record at flush. A sealed pack is immutable — the host never stamps —
// so the doc-17 contract applies: the PRODUCER stamps the host arrival id
// itself, before seal:
//
//     b.add_i64("$seq", (int64_t)xi::run_id());
//
// xi::run_id() is the same value the Record path injected (assigned at
// dequeue, monotonic on the wire in arrival mode), and the staged push flush
// (use_push_pack_cb -> EmitTurn gate) guarantees DELIVERY order independently.
// So the wire `seq` the driver reads is monotonic in receive order under
// arrival mode and interleaved under completion mode — exactly what
// qa_result_order proved with the host stamp.
//
// Doctrine probe (doc 17 §b): use(sink).process(pack) on the declared sink is
// REJECTED (-5 -> empty pack + once-per-name log) — process() is request-reply,
// the sink feed is push(). Each frame records the rejection in the pushed pack
// (`probe_rejected`), asserted by the driver on every frame.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <chrono>
#include <thread>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    int ms = (frame % 5 == 0) ? 45 : 4;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));

    // U3 doctrine probe: process(pack) on a declared ordered sink -> empty.
    xi::ScriptPackBuilder pb;
    pb.add_i64("probe", 1);
    auto probe = pb.seal();
    const bool rejected = probe.valid() && !xi::use("expose").process(probe).valid();

    xi::ScriptPackBuilder b;
    b.add_str("$channel", "order");
    b.add_i64("$seq", (int64_t)xi::run_id());   // producer-stamped host arrival id
    b.add_i64("seq", frame);                    // frame-derived marker (secondary)
    b.add_i64("probe_rejected", rejected ? 1 : 0);
    auto pack = b.seal();
    if (pack.valid()) xi::use("expose").push(pack);   // staged; flushed in frame order
}
