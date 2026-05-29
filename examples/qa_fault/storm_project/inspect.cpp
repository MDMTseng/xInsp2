//
// inspect.cpp — qa_fault_storm (QF-I7 fixture). Drives the armed "crasher"
// instance every frame, so each backend start crashes on its first autostart
// inspect. The FE respawns until the 5-in-60s cap, then stays safe. The driver
// counts ENTER SAFE STATE lines for exact respawn accounting (safety prop SP2).
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& crasher = xi::use("crasher");
    auto out = crasher.process(xi::Record().set("frame", frame));
    VAR(count, out["count"].as_int(-1));
}
