// inspect_v1.cpp — counts xi_inspect_entry invocations, persists count
// in xi::state() across hot-reloads. Reads a tunable threshold via
// xi::Param<int>.
//
// Output path: VAR was removed from core, so the script surfaces its
// computed values to the `expose` sink under channel "main" (the driver
// subscribes to it and decodes the XEX1 frames). Record key order is the
// display order; "$channel" selects the channel.

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
