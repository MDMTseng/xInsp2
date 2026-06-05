// qa_two_group_paths — proves two sources route down two independent group lanes.
//
// Two frame_source instances self-emit triggers: src_fast (instance group "fast",
// 40fps) and src_slow (group "slow", 8fps). The dispatcher stamps each event's
// `group` from the EMITTING instance's instance.json "group", so the two streams
// never share a lane. This script just emits a Result tagging which source fired;
// the driver buckets the run_result events by their `group` field and checks the
// two paths are independent (right routing, each at its own cadence).
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>
#include <chrono>
#include <thread>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    if (!t.is_active()) return;   // skip the synthetic timer tick (no source)

    const std::string src = t.primary_source();
    EMIT(src);   // surface the existing var (VAR(src,src) would redeclare it → C2374)
    // A little work so the lanes actually overlap (fast group has 3 threads).
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    // Result message = the emitting source; the dispatcher tags run_result.group.
    xi::ok(1, src);
}
