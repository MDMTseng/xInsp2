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
// VAR set per event:
//   src, src_name, seq, n_sources, has_img, latency_us, sink_total,
//   sink_for_src, slow_path
//
// Reentrancy: zero script-level mutable state. xi::current_trigger() is
// thread-local; the sink plugin (work_detector) is reentrant by
// construction (atomic counters + mutex around the per-tag map).
//
// Cross-platform: only <chrono>/<cstdint>/<cstring>/<string>; no Win32.

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include <chrono>
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

constexpr uint64_t TAG_A = fnv1a64("source_a");
constexpr uint64_t TAG_B = fnv1a64("source_b");

const char* tag_to_str(uint64_t t) {
    if (t == TAG_A) return "a";
    if (t == TAG_B) return "b";
    return "unknown";
}

} // namespace

// File-scope params (script-tunable from the driver via cmd:set_param).
xi::Param<int> g_slow_mode_ms("slow_mode_ms", 0);

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    VAR(active, t.is_active());
    if (!t.is_active()) return;

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

    // Per-source per-call sleep override on the sink. source_b
    // can be made deliberately heavy via slow_mode_ms, which is the
    // hook the watchdog-under-N>1 test pokes.
    // NB: this used to read `bool slow_path = ...; VAR(slow_path, slow_path);`
    // which is the natural pattern flagged in docs/guides/writing-a-script.md
    // ("Gotcha - VAR(name, ...) declares a local") and the cross-reference in
    // backend/include/xi/xi_var.hpp. Inline the expression instead, so the
    // VAR macro owns the binding outright.
    int slow_ms = g_slow_mode_ms.get();
    VAR(slow_path, (src_tag == TAG_B) && (slow_ms > 0));

    auto& det = xi::use("sink");
    auto rec = xi::Record().image("img", img);
    if (slow_path) {
        rec.set("sleep_ms", slow_ms);
    }
    auto out = det.process(rec);
    VAR(sink_total,   out["processed_total"].as_int(-1));
    VAR(sink_for_src, out["processed_for_src"].as_int(-1));

    using clk = std::chrono::system_clock;
    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                      clk::now().time_since_epoch()).count();
    int64_t lat = now_us - ts_emit;
    if (lat < 0) lat = 0;
    VAR(latency_us, (double)lat);
}
