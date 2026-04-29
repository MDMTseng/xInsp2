//
// inspect.cpp — hot_reload_run2 (v1).
//
// Frame counter. Increments xi::state()["count"] each call. Reads a
// tunable threshold via xi::Param<int>. No external image source.
//
// VAR(name, expr) declares a local `name` in the enclosing scope, so
// we can't have an existing `int count` AND a `VAR(count, count)`. We
// inline reads of state directly inside the VAR macro args.
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
}
