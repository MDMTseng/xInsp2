// inspect_v1.cpp — counts xi_inspect_entry invocations, persists count
// in xi::state() across hot-reloads. Reads a tunable threshold via
// xi::Param<int>.

#include <xi/xi.hpp>

static xi::Param<int> threshold{"threshold", 100, {0, 1000}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto& s = xi::state();
    int new_count = s["count"].as_int(0) + 1;
    s.set("count", new_count);

    int t = (int)threshold;

    VAR(count, new_count);
    VAR(threshold, t);
    VAR(triggered, new_count >= t);
}
