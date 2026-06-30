//
// inspect.cpp — hot_reload_run2 staging copy (the driver overwrites this with
// inspect_v1.cpp then inspect_v2.cpp). Kept in sync with inspect_v1.cpp so a
// manual compile of this file matches the migrated (expose-based) output path.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

static xi::Param<int> threshold_p{"threshold", 100, {0, 100000}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    int next = xi::state()["count"].as_int(0) + 1;
    xi::state().set("count", next);

    int t_val = threshold_p;

    xi::Record rec;
    rec.set("count", next)
       .set("threshold", t_val)
       .set("triggered", next >= t_val);
    rec.set("$channel", "main");
    xi::use("expose").process(rec);
}
