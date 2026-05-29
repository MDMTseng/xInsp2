//
// inspect.cpp — fe_supervisor_healthy.
//
// The "counter" instance is configured armed:false, so process() just returns
// a running count and never crashes. Under FE autostart this gives a healthy,
// long-running line — the positive counterpart to examples/fe_supervisor
// (which arms the same plugin to crash). Used to prove the FE runs a healthy
// backend, shuts it down cleanly, and never orphans it.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& counter = xi::use("counter");
    auto out = counter.process(xi::Record().set("frame", frame));
    VAR(count, out["count"].as_int(-1));
}
