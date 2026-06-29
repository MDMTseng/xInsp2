//
// dll_version_clash — the script is a no-op; the experiment is driven from
// run_experiment.py via exchange_instance on probeA / probeB.
//
#include <xi/xi.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    // intentionally empty — see run_experiment.py
}
