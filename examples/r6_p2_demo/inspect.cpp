// inspect.cpp — r6_p2_demo: smoke-tests the P2-2 / P2-5 trigger
// accessors. Runs against the burst_source plugin re-used from
// cross_proc_trigger; the source instance is named "src_main".
//
// VARs emitted per trigger:
//   primary       — t.primary_source()  (expect "src_main")
//   has_main      — t.has_source("src_main") (expect true)
//   has_other     — t.has_source("nope") (expect false)
//   n_sources     — sources().size()    (expect 1)

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include <string>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    VAR(active, t.is_active());
    if (!t.is_active()) return;

    VAR(primary,   t.primary_source());
    VAR(has_main,  t.has_source("src_main"));
    VAR(has_other, t.has_source("nope"));
    VAR(n_sources, (int)t.sources().size());
}
