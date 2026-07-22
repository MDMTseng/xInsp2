// qa_runtime_settings — source-less: the synthetic timer drives every run, so the
// run_result rate == the live timer rate. Used to verify set_timer_fps (live) +
// project.json runtime.timer_fps + set_process_priority.
#include <xi/xi.hpp>
#include <xi/xi_result.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) { xi::ok(1, "tick"); }
