//
// inspect.cpp — qa_fault_storm (QF-I7 fixture). Drives the armed "crasher"
// instance every frame, so each backend start crashes on its first autostart
// inspect. The FE respawns until the 5-in-60s cap, then stays safe. The driver
// counts ENTER SAFE STATE lines for exact respawn accounting (safety prop SP2).
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::ScriptPackBuilder cb;
    cb.add_i64("frame", frame);
    auto out = xi::use("crasher").process(cb.seal());
    // Surface the crasher's count through `expose` (channel "qa").
    xi::ScriptPackBuilder b;
    b.add_str("$channel", "qa");
    b.add_i64("count", out.get_i64("count").value_or(-1));
    xi::use("expose").push(b.seal());
}
