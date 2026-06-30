// inspect.cpp — burst_pipeline inspection script.
//
// On every trigger event:
//   1. Read the source frame from xi::current_trigger().image("source0").
//   2. Simulate ~30 ms of "heavy" upstream CV work (sleep — stand-in
//      for the real cvtColor / blur / matchTemplate pipeline that a
//      production case would have here).
//   3. Forward the frame to the light_classifier plugin (sleeps 5 ms,
//      returns {seq, kind}).
//   4. Compute end-to-end latency from the trigger's emit timestamp.
//   5. Surface seq, kind, latency_us, classifier_processed (and the
//      active/has_img guards) to the `expose` sink on channel "pipe".
//
// VAR() was the old data-out path but the core no longer publishes it
// (the Python SDK is generic now). The driver reads what we compute by
// subscribing to the expose channel and decoding the XEX1 frames, so we
// push one xi::Record per inspect (including timer-only ticks, which
// carry active=false) to xi::use("expose").process(rec). The channel id
// rides under the reserved "$channel" key; record key order is display
// order.
//
// Reentrancy: this entire body runs under N dispatch threads. The
// xi:: ops here (current_trigger, use, VAR) are documented as
// thread-safe; we hold no script-level shared state. The classifier
// plugin is reentrant by construction (only state is atomic counter).
// xi::state() is NOT touched.
//
// Heavy-work sleep lives HERE not in the source plugin. Source emits
// off a worker thread and the script body is what dispatch_threads
// fans out across. Putting the sleep in the source would just stall
// emission; putting it here lets N inspect threads overlap N sleeps.

#include <xi/xi.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_use.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    if (!t.is_active()) {
        // Timer-driven tick before any source has emitted — surface it
        // as an inactive frame so the driver can count timer-only ticks.
        xi::Record tick;
        tick.set("active", false).set("$channel", "pipe");
        xi::use("expose").process(tick);
        return;
    }

    auto img = t.image("source0");
    if (img.empty() || img.data() == nullptr) {
        xi::Record r;
        r.set("active", true).set("has_img", false).set("$channel", "pipe");
        xi::use("expose").process(r);
        return;
    }

    // Decode seq embedded in the first 8 bytes (uint64 LE).
    uint64_t decoded_seq = 0;
    std::memcpy(&decoded_seq, img.data(), sizeof(decoded_seq));
    const int seq = (int)(decoded_seq & 0x7fffffff);

    int64_t ts_emit = t.timestamp_us();

    // Simulate ~30 ms of heavy upstream CV work.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Hand off to the cheap classifier (sleeps 5 ms inside).
    auto& clf = xi::use("classifier");
    auto out = clf.process(xi::Record()
        .image("img", img)
        .set("sleep_ms", 5));

    // End-to-end latency: now - source emit timestamp (microseconds).
    using clk = std::chrono::system_clock;
    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                      clk::now().time_since_epoch()).count();
    int64_t lat = now_us - ts_emit;
    if (lat < 0) lat = 0;     // clock skew safety; shouldn't happen

    // Surface everything the driver asserts on to the expose sink. Key
    // order here is the display order; the driver reads these out of the
    // decoded frame's "values".
    xi::Record r;
    r.set("active", true)
     .set("has_img", true)
     .set("seq", seq)
     .set("emit_ts_us", (double)ts_emit)
     .set("kind", out["kind"].as_int(-1))
     .set("classifier_seq", out["seq"].as_int(-1))
     .set("classifier_processed", out["processed"].as_int(-1))
     .set("latency_us", (double)lat)
     .set("$channel", "pipe");
    xi::use("expose").process(r);
}
