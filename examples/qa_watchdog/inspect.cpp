//
// inspect.cpp — qa_watchdog. A runaway inspect that ignores cooperative cancel:
// a busy loop that never polls xi::cancellation_requested() and never returns.
// Under parallelism.dispatch_threads=4 every worker that picks up a frame wedges
// here, so this exercises the PER-WORKER watchdog: the monitor must detect an
// overrun on a worker slot (not just the single legacy slot), fail to cancel it
// cooperatively, and hard-trip -> the backend exits for the FE to respawn.
//
#include <xi/xi.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    VAR(start, frame);
    volatile long long sink = 0;
    while (true) sink = sink + 1;   // never returns, never checks cancel
}
