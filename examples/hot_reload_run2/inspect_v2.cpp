//
// inspect.cpp — hot_reload_run2 (v2).
//
// Same state field + same Param. Adds version=2 and half_count.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

static xi::Param<int> threshold_p{"threshold", 100, {0, 100000}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    int next = xi::state()["count"].as_int(0) + 1;
    xi::state().set("count", next);

    int t_val = threshold_p;

    VAR(count, next);
    VAR(threshold, t_val);
    VAR(triggered, next >= t_val);
    VAR(version, 2);
    VAR(half_count, next / 2);
}
