//
// inspect.cpp — qa_race crashburst project.
//
// One call into the armed "crasher" instance. The instance is configured
// armed:true, so the very first process() spawns a raw std::thread that
// null-derefs and takes the backend down on frame 0 — driving the FE
// supervisor's detect-death -> safe-state -> respawn -> cap loop as fast as
// the respawn backoff allows. Copied/adapted from examples/fe_supervisor.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& crasher = xi::use("crasher");
    auto out = crasher.process(xi::Record().set("frame", frame));
    VAR(count, out["count"].as_int(-1));
}
