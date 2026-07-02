//
// inspect.cpp — plugin_crash_forensics, single-stage script.
//
// Plumbs one call into the "crasher" instance. On a normal frame it
// returns a running count; once the driver has armed the instance via
// exchange, the next call hard-crashes the backend from inside a raw
// plugin thread. This script does nothing special — the dispatch
// thread's crash breadcrumb (last_cmd="inspect", last_phase="inspect")
// is what the driver checks for in the resulting crash report.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& crasher = xi::use("crasher");
    auto out = crasher.process(xi::Record().set("frame", frame));

    // Surface the running count (and any error) through `expose` on channel "crash".
    xi::Record rec;
    rec.set("$channel", "crash").set("count", out["count"].as_int(-1));
    auto err = out["error"].as_string("");
    if (!err.empty()) {
        rec.set("error", err);
    }
    xi::use("expose").process(rec);
}
