// inspect_v2.cpp — same shape as v1, plus a `version` VAR and a
// `half_count` VAR so the driver can tell which DLL is live.

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
    VAR(version, 2);
    VAR(half_count, new_count / 2);
}
