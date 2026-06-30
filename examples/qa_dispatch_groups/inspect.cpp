// Minimal workload for the dispatch-groups smoke test. Continuous timer ticks
// route to the default group ("high"). VAR was purged from the core, so the
// body just needs to run: the driver proves a lane fired by counting the
// backend's run_finished / run_result lifecycle events (emitted per inspect,
// independent of any script output).
#include <xi/xi.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) { (void)frame; }
