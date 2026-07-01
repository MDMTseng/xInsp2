// inspect.cpp — parallel_inspect_demo: simulates a slow per-frame
// CV pipeline (sleep_for 100ms) so the parallel-dispatch effect is
// directly visible in throughput.
//
// Each trigger event fires xi_inspect_entry on whichever dispatcher
// thread the host's pool picked. With N sources firing in lockstep
// (simulated hardware-synchronized cameras) and dispatch_threads=N,
// the wall-clock inspect time is ~100ms total instead of ~N*100ms.
//
// VAR was removed from core. To let the driver count *active* inspects
// (the slow 100 ms ones — the metric this demo is about, as opposed to
// the cheap inactive timer ticks that also fire run_finished), each
// active inspect pushes one tiny record to the `expose` sink on channel
// "runs". The driver subscribes and counts the decoded XEX1 frames.

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include <chrono>
#include <thread>

// A4: explicit-trigger entry. The host passes the trigger in (no ambient
// thread_local), so `t` is self-contained and safe to capture by value into a
// parallel region — the exact hazard this demo's cousins used to hit.
XI_INSPECT_ENTRY(t, /*frame*/ frame) {
    (void)frame;
    if (!t.is_active()) return;

    // Simulate 100 ms of CV work. Real workloads use cv::Canny / template
    // match / a deep model — same shape: CPU-bound for tens to hundreds
    // of ms. The dispatcher pool's parallelism is what amortises the
    // wall-clock cost when N triggers arrive simultaneously.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Mark this active inspect as completed for the driver's rate count.
    xi::Record rec;
    rec.set("src", t.primary_source()).set("$channel", "runs");
    xi::use("expose").process(rec);
}
