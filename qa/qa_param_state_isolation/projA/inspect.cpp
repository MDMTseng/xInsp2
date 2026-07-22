// projA/inspect.cpp — param-isolation regression, project A.
//
// Declares a tunable `thresh` (default 50). The driver opens this project,
// set_param's thresh=200, then switches to project B. The effective value is
// read back via cmd:list_params (which reports each Param's live value).
#include <xi/xi.hpp>

static xi::Param<int> thresh{"thresh", 50, {0, 255}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    // No work needed — the param value is read via list_params. Touch it so
    // the variable is "used" and the registry slot is live.
    (void)(int)thresh;
}
