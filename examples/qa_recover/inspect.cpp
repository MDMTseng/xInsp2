//
// inspect.cpp — qa_recover regression.
//
// Drives one call into the "healer" instance, which crashes the backend its
// first `crash_count` starts and then recovers. Exercises the FE supervisor's
// recover-and-clear transition under headless autostart.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& healer = xi::use("healer");
    auto out = healer.process(xi::Record().set("frame", frame));
    // Surface the healer's count + recovery flag through `expose` (channel "qa").
    xi::use("expose").process(xi::Record()
        .set("$channel", "qa")
        .set("count", out["count"].as_int(-1))
        .set("recovered", out["recovered"].as_int(0)));
}
