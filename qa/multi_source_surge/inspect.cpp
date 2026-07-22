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
#include <xi/xi_script_pack.hpp>

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

    auto f = t.pack();
    auto img = f.get_image("img");
    if (!img || img->pixels.size() < 16) return;

    uint64_t seq_u64 = 0, src_tag = 0;
    const uint8_t* d = img->pixels.data();
    std::memcpy(&seq_u64, d,     sizeof(seq_u64));
    std::memcpy(&src_tag, d + 8, sizeof(src_tag));

    bool route_fast = (src_tag == TAG_STEADY) || (src_tag == TAG_BURST);
    bool route_slow = (src_tag == TAG_BURST)  || (src_tag == TAG_VARIABLE);

    // Chain the frame into the detectors via their xi.pack@1 door. Rebuild the
    // input pack from the borrowed frame (a sealed pack is immutable).
    auto build_det_input = [&]() {
        xi::ScriptPackBuilder b;
        b.add_image("img", img->width, img->height, img->channels, img->pixels.data());
        return b.seal();
    };
    if (route_fast) (void)xi::use("detector_fast").process(build_det_input());
    if (route_slow) (void)xi::use("detector_slow").process(build_det_input());

    // Surface per-inspect attribution for the driver's fan-in check (pack-plane).
    xi::ScriptPackBuilder rb;
    rb.add_str("$channel",  "runs");
    rb.add_str("src",       tag_to_str(src_tag));
    rb.add_str("src_name",  source);
    rb.add_i64("seq",       (int)(seq_u64 & 0x7fffffff));
    rb.add_bool("used_fast", route_fast);
    rb.add_bool("used_slow", route_slow);
    xi::use("expose").push(rb.seal());
}
