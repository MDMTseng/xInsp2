// Minimal workload for the dispatch-groups smoke test. Continuous timer ticks
// route to the default group ("high"); the script just emits a frame counter.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) { VAR(frame_n, frame); VAR(ok, true); }
