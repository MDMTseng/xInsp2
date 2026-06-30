// inspect_v2.cpp — same shape as v1, plus a `version` field and a
// `half_count` field so the driver can tell which DLL is live.
//
// Surfaces to the `expose` sink under channel "main" (VAR was removed
// from core). See inspect_v1.cpp for the output-path rationale.

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
       .set("triggered", new_count >= t)
       .set("version", 2)
       .set("half_count", new_count / 2);
    rec.set("$channel", "main");
    xi::use("expose").process(rec);
}
