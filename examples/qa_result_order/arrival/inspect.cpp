//
// inspect.cpp — qa_result_order. Uneven inspect time (every 5th frame is slow)
// so that under dispatch_threads>1 a later, faster frame finishes before an
// earlier slow one. With result_order:"arrival" the backend must still emit
// results in frame-arrival order; with "completion" they interleave.
//
// VAR was removed from core, so per-frame results are surfaced through the
// `expose` sink instead: each inspect pushes a Record on channel "order" whose
// `seq` value carries the frame number. `expose` is an ORDERED sink — the host
// stages process() and flushes it under the per-lane emit gate, stamping the
// wire run_id ($seq) onto each XEX1 frame. So the wire `seq` (= run_id) the
// driver reads via collect_frames() is monotonic in receive order under arrival
// mode and interleaved under completion mode — exactly what VAR's run_id used to
// prove.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <chrono>
#include <thread>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    int ms = (frame % 5 == 0) ? 45 : 4;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));

    xi::Record rec;
    rec.set("seq", frame);            // frame-derived marker (secondary evidence)
    rec.set("$channel", "order");
    xi::use("expose").process(rec);   // wire run_id ($seq) rides the XEX1 frame
}
