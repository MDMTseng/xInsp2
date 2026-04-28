// cancel_aware_script.cpp — pattern for long-running user code that
// honours the watchdog's cooperative cancel.
//
// cv:: calls (cv::GaussianBlur, cv::threshold, cv::Sobel, …) cannot
// be cancelled mid-call, so any user loop that runs for > a few
// seconds should poll the global cancel flag itself between op calls.
// The check is one atomic load — call it on a reasonable cadence
// (every N rows, every chunk boundary, between independent passes).
//
// When watchdog trips:
//   1. Backend sets the global cancel flag via the script DLL's
//      xi_script_set_global_cancel(1) thunk.
//   2. xi::cancellation_requested() returns true on every thread.
//   3. This script sees the flag, emits a `cancelled` VAR and exits.
//   4. The cooperative phase succeeded; backend logs
//      "cooperative cancel succeeded" and skips TerminateThread.
//
// If a script does NOT poll, the cooperative phase fails (1000 ms
// grace passes with no exit) and backend falls back to the
// TerminateThread fallback — which works but logs a louder warning
// and leaves outstanding pool refs to be swept by the owner ledger.
//
// Trigger this in practice with:
//   set xinsp2.watchdogMs = 5000  (extension setting)
//   run a loop that takes > 5 s
// You should see the inspect exit with `cancelled = true` instead of
// being TerminateThread'd.
//

#include <xi/xi.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto img = xi::imread(xi::current_frame_path());
    if (img.empty()) {
        VAR(error, std::string("frame load failed; pass --frame-path"));
        return;
    }

    // Pretend to do something expensive: a simple per-pixel scan
    // looped many times so the run takes long enough for a
    // moderately-set watchdog to trip.
    int64_t accum = 0;
    const int passes = 200;
    for (int p = 0; p < passes; ++p) {
        // Cancellation poll on the OUTER loop is the cheap default —
        // 200 atomic loads total even on a 20 MP image. Inner loops
        // typically don't need it unless they themselves take seconds.
        if (xi::cancellation_requested()) {
            VAR(cancelled, true);
            VAR(passes_done, p);
            VAR(accum, accum);
            return;
        }
        for (int y = 0; y < img.height; ++y) {
            for (int x = 0; x < img.width; ++x) {
                accum += img.data()[y * img.width * img.channels + x];
            }
        }
    }

    VAR(cancelled,    false);
    VAR(passes_done,  passes);
    VAR(accum,        accum);
    VAR(input,        img);
}
