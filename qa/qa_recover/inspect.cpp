//
// inspect.cpp — qa_recover regression.
//
// Drives one call into the "healer" instance, which crashes the backend its
// first `crash_count` starts and then recovers. Exercises the FE supervisor's
// recover-and-clear transition under headless autostart.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::ScriptPackBuilder ib;
    ib.add_i64("frame", frame);
    auto out = xi::use("healer").process(ib.seal());   // drive healer's xi.pack@1 door
    // Surface the healer's count + recovery flag through `expose` (channel "qa").
    xi::ScriptPackBuilder rb;
    rb.add_str("$channel", "qa");
    rb.add_i64("count", out.get_i64("count").value_or(-1));
    rb.add_i64("recovered", out.get_i64("recovered").value_or(0));
    xi::use("expose").push(rb.seal());
}
