//
// inspect.cpp — fe_supervisor regression.
//
// Plumbs one call into the "crasher" instance. The instance is configured
// `armed: true`, so the very first process() spawns a raw thread that
// null-derefs and takes the backend down — exercising the FE supervisor's
// detect-death -> safe-state -> respawn loop under headless autostart.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& crasher = xi::use("crasher");
    auto out = crasher.process(xi::Record().set("frame", frame));
    VAR(count, out["count"].as_int(-1));
}
