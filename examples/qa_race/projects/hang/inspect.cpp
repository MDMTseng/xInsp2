//
// inspect.cpp — qa_race hang project (FE-E4 PortUnresponsive design fixture).
//
// Calls into the "hanger" instance, whose process() sleeps forever. Under
// autostart this wedges a DISPATCH thread, not the WS poll thread — see
// PLAN.md "FE-E4 PortUnresponsive — design". On its own this does NOT make the
// port stop accepting, which is why RACE-FE4 is a documented manual stub.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& hanger = xi::use("hanger");
    auto out = hanger.process(xi::Record().set("frame", frame));
    VAR(count, out["count"].as_int(-1));
}
