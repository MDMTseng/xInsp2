// inspect.cpp — staging copy (the driver overwrites this with inspect_v1.cpp
// then inspect_v2.cpp). Kept in sync with inspect_v1.cpp so a manual compile
// of this file matches the migrated (expose-based) output path.

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

static xi::Param<int> threshold{"threshold", 100, {0, 1000}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto& s = xi::state();
    int new_count = s["count"].as_int(0) + 1;
    s.set("count", new_count);

    int t = (int)threshold;

    xi::Record rec;
    rec.set("count", new_count)
       .set("threshold", t)
       .set("triggered", new_count >= t);
    rec.set("$channel", "main");
    xi::use("expose").process(rec);
}
