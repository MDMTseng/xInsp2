// inspect.cpp — parallel_inspect_demo: simulates a slow per-frame
// CV pipeline (sleep_for 100ms) so the parallel-dispatch effect is
// directly visible in latency.
//
// Each trigger event fires xi_inspect_entry on whichever dispatcher
// thread the host's pool picked. With N sources firing in lockstep
// (simulated hardware-synchronized cameras) and dispatch_threads=N,
// the wall-clock inspect time is ~100ms total instead of ~N*100ms.

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include <chrono>
#include <thread>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    VAR(active, t.is_active());
    if (!t.is_active()) return;

    int64_t t_start = xi::now_us();
    VAR(src,         t.primary_source());
    VAR(emit_ts_us,  (double)t.timestamp_us());
    VAR(start_ts_us, (double)t_start);

    // Simulate 100 ms of CV work. Real workloads use cv::Canny / template
    // match / a deep model — same shape: CPU-bound for tens to hundreds
    // of ms. The dispatcher pool's parallelism is what amortises the
    // wall-clock cost when N triggers arrive simultaneously.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int64_t t_end = xi::now_us();
    VAR(end_ts_us, (double)t_end);
    VAR(inspect_us, (double)(t_end - t_start));
}
