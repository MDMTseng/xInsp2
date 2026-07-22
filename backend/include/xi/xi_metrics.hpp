// xi_metrics.hpp — minimal observability export (OQ-7a, core_fix_plan §21 / §27.5).
//
// The SMALLEST useful metrics surface, deliberately NOT a telemetry stack
// (Part IV §27.5 "don't gold-plate the partial gaps"):
//
//   * monotonic frame COUNTERS   — total / ok / error (process-uptime cumulative,
//                                  never reset; a monitor derives throughput itself).
//   * a fixed-bucket COMPUTE     — per-frame script inspect COMPUTE time, lock-free
//     HISTOGRAM                    atomics only (the codebase favours atomics +
//                                  solved-lock patterns; nothing to lock here).
//                                  NOTE: this is inspect compute ONLY — it excludes
//                                  queue wait, emit-gate wait, staged sink flush,
//                                  JPEG encode and WS send. Exported under the key
//                                  `inspect_compute_ms` (was `latency_ms`; renamed
//                                  per external review 05 #7 so consumers can't
//                                  misread it as cycle/decision latency).
//
// Recorded once per inspection in run_one_inspection (service_main.cpp, right
// where the per-frame `dt_ms` is already computed) and exported as a JSON blob
// over the EXISTING WS command channel (`cmd:metrics`, alongside dispatch_stats /
// image_pool_stats) — no new server, no new transport. Point-query snapshot; the
// caller diffs successive snapshots to get rates, exactly like dispatch_stats.
//
// Histogram buckets are NON-cumulative (each bucket counts frames that fell in
// [prev_bound, this_bound), NOT frames <= bound); the reader sums buckets to
// compute a CDF / p-quantile estimate. Each bucket is keyed on the wire by
// `bucket_ms` = its upper bound (NOT Prometheus `le`, which would imply the
// counts are cumulative — they are not). Edges are milliseconds, chosen to span
// a machine-vision frame budget (sub-ms fast paths up to multi-second stalls).
//
#ifndef XI_METRICS_HPP
#define XI_METRICS_HPP

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>

namespace xi {

class MetricsRegistry {
public:
    // Upper bounds (inclusive) of the latency buckets, in milliseconds. A frame
    // with latency <= edge[i] and > edge[i-1] increments bucket i; anything
    // larger than the last edge lands in the +inf overflow bucket (index kBuckets).
    static constexpr double kEdgesMs[] = {
        0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000
    };
    static constexpr std::size_t kBuckets = sizeof(kEdgesMs) / sizeof(kEdgesMs[0]);

    static MetricsRegistry& instance() {
        static MetricsRegistry m;
        return m;
    }

    // Record one completed inspection. `latency_ms` is the wall time of the run;
    // `ok` distinguishes a clean finish from a crash/throw. Lock-free — safe to
    // call from any dispatch worker concurrently. relaxed ordering is fine: these
    // are independent counters, no happens-before is published through them.
    // Because each counter is bumped separately (frames_total_ FIRST, its bucket
    // LAST), a concurrent snapshot_json read is NOT a consistent cut — see the note
    // there. It is self-healing once all record_frame calls have returned.
    void record_frame(double latency_ms, bool ok) noexcept {
        frames_total_.fetch_add(1, std::memory_order_relaxed);
        (ok ? frames_ok_ : frames_error_).fetch_add(1, std::memory_order_relaxed);

        // Sum kept in integer microseconds so the accumulator is exact and
        // wrap-safe over a long uptime (uint64 µs ≈ 584k years).
        if (latency_ms < 0) latency_ms = 0;
        uint64_t us = (uint64_t)(latency_ms * 1000.0 + 0.5);
        latency_sum_us_.fetch_add(us, std::memory_order_relaxed);

        std::size_t b = kBuckets;                 // default: +inf overflow
        for (std::size_t i = 0; i < kBuckets; ++i) {
            if (latency_ms <= kEdgesMs[i]) { b = i; break; }
        }
        buckets_[b].fetch_add(1, std::memory_order_relaxed);
    }

    // Serialize a point-in-time snapshot as a JSON object into `out` (appended).
    // Shape (stable, documented in ws-protocol.md):
    //   {"frames_total":N,"frames_ok":N,"frames_error":N,
    //    "inspect_compute_ms":{"count":N,"sum_ms":F,"mean_ms":F,
    //      "buckets":[{"bucket_ms":0.5,"count":n},...,{"bucket_ms":"inf","count":n}]}}
    //
    // Each bucket's `bucket_ms` is its NON-cumulative upper bound: `count` is frames
    // in [prev_bound, bucket_ms), NOT frames <= bucket_ms. (Earlier this key was `le`,
    // the Prometheus name for a CUMULATIVE ≤ edge — a mislabel, since these counts are
    // per-bucket occupancy. Renamed to bucket_ms so a standard Prometheus consumer
    // can't silently misread every quantile. WIRE-CONTRACT change; a consumer bound
    // to `le` was already getting wrong data.)
    //
    // NON-ATOMIC multi-counter read: each counter is loaded at an independent instant
    // with relaxed ordering, and record_frame bumps frames_total_ first then its
    // bucket last, so a snapshot taken WHILE frames are mid-record_frame can show
    // Σbuckets < count and frames_ok+frames_error < frames_total (each lagging total
    // by up to the number of in-flight record_frame calls). Bounded and self-healing:
    // the snapshot is a consistent cut only after all workers have joined/quiesced.
    //
    // BREAKING (staged, not on master): the histogram key was `latency_ms`. It was
    // renamed to `inspect_compute_ms` because the value is script inspect COMPUTE
    // time only — it excludes queue wait, emit-gate wait, staged sink flush, JPEG
    // encode and WS send. The old `latency_ms` name misread as cycle/decision time
    // (external review 05 P0/#7).
    void snapshot_json(std::string& out) const {
        uint64_t total = frames_total_.load(std::memory_order_relaxed);
        uint64_t ok    = frames_ok_.load(std::memory_order_relaxed);
        uint64_t err   = frames_error_.load(std::memory_order_relaxed);
        uint64_t sumus = latency_sum_us_.load(std::memory_order_relaxed);

        out += "{\"frames_total\":" + std::to_string(total);
        out += ",\"frames_ok\":" + std::to_string(ok);
        out += ",\"frames_error\":" + std::to_string(err);
        out += ",\"inspect_compute_ms\":{\"count\":" + std::to_string(total);
        out += ",\"sum_ms\":" + fmt3(sumus / 1000.0);
        out += ",\"mean_ms\":" + fmt3(total ? (double)sumus / 1000.0 / (double)total : 0.0);
        out += ",\"buckets\":[";
        for (std::size_t i = 0; i < kBuckets; ++i) {
            if (i) out += ",";
            out += "{\"bucket_ms\":" + fmt3(kEdgesMs[i]) + ",\"count\":" +
                   std::to_string(buckets_[i].load(std::memory_order_relaxed)) + "}";
        }
        out += ",{\"bucket_ms\":\"inf\",\"count\":" +
               std::to_string(buckets_[kBuckets].load(std::memory_order_relaxed)) + "}";
        out += "]}}";
    }

    // Test-only: zero everything. NOT called on cmd:start — counters are
    // uptime-cumulative by design (see header note); only tests reset.
    void reset() noexcept {
        frames_total_.store(0, std::memory_order_relaxed);
        frames_ok_.store(0, std::memory_order_relaxed);
        frames_error_.store(0, std::memory_order_relaxed);
        latency_sum_us_.store(0, std::memory_order_relaxed);
        for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
    }

    uint64_t frames_total() const noexcept { return frames_total_.load(std::memory_order_relaxed); }
    uint64_t frames_ok()    const noexcept { return frames_ok_.load(std::memory_order_relaxed); }
    uint64_t frames_error() const noexcept { return frames_error_.load(std::memory_order_relaxed); }
    uint64_t bucket(std::size_t i) const noexcept { return buckets_[i].load(std::memory_order_relaxed); }

private:
    MetricsRegistry() = default;

    // Minimal fixed(3)-decimal formatter — avoids sprintf locale surprises and
    // keeps the JSON compact (std::to_string(double) emits 6 trailing digits).
    static std::string fmt3(double v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3f", v);
        return std::string(buf);
    }

    std::atomic<uint64_t> frames_total_{0};
    std::atomic<uint64_t> frames_ok_{0};
    std::atomic<uint64_t> frames_error_{0};
    std::atomic<uint64_t> latency_sum_us_{0};
    std::atomic<uint64_t> buckets_[kBuckets + 1]{};   // +1 = +inf overflow
};

// Out-of-line constexpr definition (pre-C++17 ODR; harmless under C++17+).
#if !defined(__cpp_inline_variables)
constexpr double MetricsRegistry::kEdgesMs[];
#endif

} // namespace xi

#endif // XI_METRICS_HPP
