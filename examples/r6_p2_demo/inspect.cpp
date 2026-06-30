// inspect.cpp — r6_p2_demo: smoke-tests the P2-2 / P2-5 trigger
// accessors. Runs against the burst_source plugin re-used from
// cross_proc_trigger; the source instance is named "src_main".
//
// VAR was removed from core, so the accessor values are surfaced to the
// driver via the `expose` sink instead. Each active trigger pushes one
// record on channel "probe" carrying:
//   primary       — t.primary_source()        (expect "src_main")
//   has_main      — t.has_source("src_main")  (expect true)
//   has_other     — t.has_source("nope")      (expect false)
//   n_sources     — sources().size()          (expect 1)
// The driver subscribes to "probe" and decodes the XEX1 frames.

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include <string>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    if (!t.is_active()) return;

    xi::Record rec;
    rec.set("primary",   t.primary_source())
       .set("has_main",  t.has_source("src_main"))
       .set("has_other", t.has_source("nope"))
       .set("n_sources", (int)t.sources().size())
       .set("$channel",  "probe");
    xi::use("expose").process(rec);
}
