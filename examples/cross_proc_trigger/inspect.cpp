// inspect.cpp — cross_proc_trigger validation script.
//
// One source instance running in an isolated worker process emits
// triggers at 60 Hz via host->emit_trigger. The driver opens the
// project, starts continuous mode, and during a 1-second silent
// window (no backend->worker RPCs) verifies that triggers still
// flow through the worker's pipe to the host's TriggerBus.
//
// This script's only job is to prove the dispatch pipeline fired by
// surfacing one record per trigger so the driver can count them.
//
// VAR() was the old data-out path, but the core no longer publishes it
// (the Python SDK is generic now). Instead we push an xi::Record to the
// `expose` sink on channel "trig". The `expose` instance is in_process
// (host side); calling it does NOT issue a backend->worker RPC into the
// isolated source, so the driver's "silent window" invariant holds.
// The channel rides under the reserved "$channel" key.

#include <xi/xi.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_use.hpp>

#include <cstdint>
#include <string>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    if (!t.is_active()) {
        xi::Record tick;
        tick.set("active", false).set("$channel", "trig");
        xi::use("expose").process(tick);
        return;
    }

    auto srcs = t.sources();
    std::string source = srcs.empty() ? std::string{} : srcs.front();

    // Per-source seq is the lower 64 bits of the trigger id (see
    // plugins/burst_source/src/plugin.cpp tid construction).
    uint64_t tid_lo = (uint64_t)t.id().lo;

    xi::Record r;
    r.set("active", true)
     .set("src", source)
     .set("seq", (int)(tid_lo & 0x7fffffff))
     .set("$channel", "trig");
    xi::use("expose").process(r);
}
