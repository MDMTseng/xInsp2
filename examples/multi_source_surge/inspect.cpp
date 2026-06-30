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
//   4. Surface a tiny per-inspect record to the `expose` sink (channel
//      "runs") so the driver can confirm fan-in correctness — that every
//      source got routed and which detector(s) each inspect used:
//        src                — short string identifying the source
//        seq                — frame seq from the source
//        used_fast/used_slow — which detector(s) ran
//      (The former latency/queue/inspect-timing VARs were pure
//      observability and were dropped with the VAR model; throughput is
//      now measured from the backend's `run_finished` events.)
//
// Reentrancy: no script-level mutable state. xi::current_trigger() is
// thread-local (each dispatch thread sees its own slot via the host
// trigger callbacks). Two detector instances are reentrant by
// construction (atomic counters + small mutex on per-src map). The
// expose sink is reentrant (mutex-guarded).

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

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
    if (!t.is_active()) return;

    // Find the source name in this trigger's set. POLICY_ANY means
    // exactly one entry on each event; we still iterate to be robust.
    auto srcs = t.sources();
    if (srcs.empty()) return;
    const std::string& source = srcs[0];

    auto img = t.image(source);
    if (img.empty() || img.data() == nullptr) return;

    uint64_t seq_u64 = 0, src_tag = 0;
    std::memcpy(&seq_u64, img.data(),     sizeof(seq_u64));
    std::memcpy(&src_tag, img.data() + 8, sizeof(src_tag));

    bool route_fast = (src_tag == TAG_STEADY) || (src_tag == TAG_BURST);
    bool route_slow = (src_tag == TAG_BURST)  || (src_tag == TAG_VARIABLE);

    if (route_fast) {
        auto& det = xi::use("detector_fast");
        (void)det.process(xi::Record().image("img", img));
    }
    if (route_slow) {
        auto& det = xi::use("detector_slow");
        (void)det.process(xi::Record().image("img", img));
    }

    // Surface per-inspect attribution for the driver's fan-in check.
    xi::Record rec;
    rec.set("src",       std::string(tag_to_str(src_tag)))
       .set("src_name",  source)
       .set("seq",       (int)(seq_u64 & 0x7fffffff))
       .set("used_fast", route_fast)
       .set("used_slow", route_slow)
       .set("$channel",  "runs");
    xi::use("expose").process(rec);
}
