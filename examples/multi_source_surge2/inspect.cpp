// inspect.cpp - multi_source_surge2 dispatch script.
//
// FL r6 regression: same FNV-1a tag-routing trick as multi_source_surge,
// trimmed to 2 sources + 1 sink. Per-event work configurable via the
// optional `slow_mode` xi::Param so we can deliberately exercise the
// watchdog warning path under N>1 by raising per-call cost.
//
// Routing:
//   source_a (steady 50Hz)  -> sink, light work
//   source_b (bursty 25Hz)  -> sink, optional heavy sleep (`slow_mode_ms`)
//
// This is a pure dispatch/throughput demo: it routes frames to the sink
// and lets source_b be made heavy for the watchdog test. It produces no
// script-level observability of its own (the former per-event VAR set was
// dropped when the VAR model was removed from core); the driver measures
// throughput from the backend's `run_finished` events instead.
//
// Reentrancy: zero script-level mutable state. xi::current_trigger() is
// thread-local; the sink plugin (work_detector) is reentrant by
// construction (atomic counters + mutex around the per-tag map).
//
// Cross-platform: only <cstdint>/<cstring>/<string>; no Win32.

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

constexpr uint64_t fnv1a64(const char* s) {
    uint64_t h = 0xcbf29ce484222325ull;
    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= 0x100000001b3ull;
    }
    return h;
}

// Only source_b needs identifying (it carries the optional heavy path);
// source_a is just "everything else", so its tag isn't needed.
constexpr uint64_t TAG_B = fnv1a64("source_b");

} // namespace

// File-scope params (script-tunable from the driver via cmd:set_param).
xi::Param<int> g_slow_mode_ms("slow_mode_ms", 0);

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    // Gate on activity (control flow); the bare `is_active()` value was
    // VAR observability that no longer has a sink.
    if (!t.is_active()) return;

    auto srcs = t.sources();
    if (srcs.empty()) return;   // was VAR(error, "no_sources")
    const std::string& source = srcs[0];

    auto img = t.image(source);
    if (img.empty() || img.data() == nullptr) return;  // was VAR(error, "empty_image:"+source)

    uint64_t src_tag = 0;
    std::memcpy(&src_tag, img.data() + 8, sizeof(src_tag));

    // Per-source per-call sleep override on the sink. source_b can be made
    // deliberately heavy via slow_mode_ms, which is the hook the
    // watchdog-under-N>1 test pokes. This is the one observational local
    // with a real side effect (it drives `rec.set("sleep_ms", ...)`), so it
    // stays; the rest of the former VAR locals (src, seq, latency_us,
    // sink totals, has_img, n_sources) were pure observability and are gone.
    int slow_ms = g_slow_mode_ms.get();
    bool slow_path = (src_tag == TAG_B) && (slow_ms > 0);

    auto& det = xi::use("sink");
    auto rec = xi::Record().image("img", img);
    if (slow_path) {
        rec.set("sleep_ms", slow_ms);
    }
    (void)det.process(rec);   // side effect: advances the sink's counters
}
