// inspect.cpp — cross_proc_trigger validation script.
//
// One source instance running in an isolated worker process emits
// triggers at 60 Hz via host->emit_trigger. The driver opens the
// project, starts continuous mode, and during a 1-second silent
// window (no backend->worker RPCs) verifies that triggers still
// flow through the worker's pipe to the host's TriggerBus.
//
// This script's only job is to prove the dispatch pipeline fired
// by emitting one VAR per trigger so the driver can count them.

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include <cstdint>
#include <string>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    VAR(active, t.is_active());
    if (!t.is_active()) return;

    auto srcs = t.sources();
    std::string source = srcs.empty() ? std::string{} : srcs.front();
    VAR(src, source);

    // Per-source seq is the lower 64 bits of the trigger id (see
    // plugins/burst_source/src/plugin.cpp tid construction).
    uint64_t tid_lo = (uint64_t)t.id().lo;
    VAR(seq, (int64_t)tid_lo);
}
