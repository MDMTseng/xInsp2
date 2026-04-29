// inspect.cpp — multi_source_surge dispatch script.
//
// On every trigger event:
//   1. Identify which source fired by reading the bytes [8..15] stamp
//      (uint64 src_tag = FNV-1a of instance name). Also reads the seq
//      from bytes [0..7].
//   2. Route based on source:
//        source_steady    → detector_fast
//        source_burst     → detector_fast AND detector_slow (overlap)
//        source_variable  → detector_slow
//   3. Each detector sleeps inside process() — the work that
//      dispatch_threads is meant to overlap.
//   4. Emit VARs naming the source so the driver can attribute every
//      inspect end-to-end:
//        src                — short string identifying source
//        seq                — frame seq from source
//        latency_us         — now - emit_ts
//        used_fast / used_slow — booleans
//        fast_total / slow_total — detector's running counter for THIS src
//
// Reentrancy: no script-level mutable state. xi::current_trigger() is
// thread-local (each dispatch thread sees its own slot via the host
// trigger callbacks). Two detector instances are reentrant by
// construction (atomic counters + small mutex on per-src map).

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

// Same FNV-1a 64-bit as plugins/burst_source/src/plugin.cpp — keep in sync.
constexpr uint64_t fnv1a64(const char* s) {
    uint64_t h = 0xcbf29ce484222325ull;
    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= 0x100000001b3ull;
    }
    return h;
}

constexpr uint64_t TAG_STEADY   = fnv1a64("source_steady");
constexpr uint64_t TAG_BURST    = fnv1a64("source_burst");
constexpr uint64_t TAG_VARIABLE = fnv1a64("source_variable");

const char* tag_to_str(uint64_t t) {
    if (t == TAG_STEADY)   return "steady";
    if (t == TAG_BURST)    return "burst";
    if (t == TAG_VARIABLE) return "variable";
    return "unknown";
}

} // namespace

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    VAR(active, t.is_active());
    if (!t.is_active()) return;

    // Find the source name in this trigger's set. POLICY_ANY means
    // exactly one entry on each event; we still iterate to be robust
    // and to surface a friction point if the assumption breaks.
    auto srcs = t.sources();
    VAR(n_sources, (int)srcs.size());
    if (srcs.empty()) {
        VAR(error, std::string("no_sources"));
        return;
    }
    const std::string& source = srcs[0];

    auto img = t.image(source);
    VAR(has_img, !img.empty());
    if (img.empty() || img.data() == nullptr) {
        VAR(error, std::string("empty_image:") + source);
        return;
    }

    uint64_t seq_u64 = 0, src_tag = 0;
    std::memcpy(&seq_u64, img.data(),     sizeof(seq_u64));
    std::memcpy(&src_tag, img.data() + 8, sizeof(src_tag));

    int64_t ts_emit = t.timestamp_us();

    VAR(src,        std::string(tag_to_str(src_tag)));
    VAR(src_name,   source);
    VAR(seq,        (int)(seq_u64 & 0x7fffffff));
    VAR(emit_ts_us, (double)ts_emit);

    bool route_fast = (src_tag == TAG_STEADY) || (src_tag == TAG_BURST);
    bool route_slow = (src_tag == TAG_BURST)  || (src_tag == TAG_VARIABLE);

    VAR(used_fast, route_fast);
    VAR(used_slow, route_slow);

    if (route_fast) {
        auto& det = xi::use("detector_fast");
        auto out = det.process(xi::Record().image("img", img));
        VAR(fast_total,   out["processed_total"].as_int(-1));
        VAR(fast_for_src, out["processed_for_src"].as_int(-1));
    }
    if (route_slow) {
        auto& det = xi::use("detector_slow");
        auto out = det.process(xi::Record().image("img", img));
        VAR(slow_total,   out["processed_total"].as_int(-1));
        VAR(slow_for_src, out["processed_for_src"].as_int(-1));
    }

    using clk = std::chrono::system_clock;
    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                      clk::now().time_since_epoch()).count();
    int64_t lat = now_us - ts_emit;
    if (lat < 0) lat = 0;
    VAR(latency_us, (double)lat);
}
