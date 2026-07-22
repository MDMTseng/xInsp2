// projB/inspect.cpp — param-isolation regression, project B.
//
// A DIFFERENT project that happens to declare a same-named `thresh` (default
// 50). After switching from project A (where thresh was tuned to 200), B must
// start from its OWN default 50 — NOT inherit A's 200 via the cross-project
// param-cache leak (Bug #8). Same default as A on purpose: the only way 200
// can appear here is the leak.
#include <xi/xi.hpp>

static xi::Param<int> thresh{"thresh", 50, {0, 255}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    (void)(int)thresh;
}
