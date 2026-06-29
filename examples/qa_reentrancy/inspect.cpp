//
// inspect.cpp — qa_reentrancy. Each frame pokes THREE probe instances:
//   * "serial"   -> concurrency_probe      (not reentrant -> host serializes)
//   * "parallel" -> concurrency_probe_rt   (reentrant -> host runs concurrent)
//   * "capped"   -> concurrency_probe_rt   (reentrant + instance.json
//                   max_concurrency=1 -> per-instance cap holds it to 1)
// Under parallelism.dispatch_threads=4 these process() calls race per instance;
// each probe accumulates the max concurrency it ever observed. The driver reads
// those accumulated counters AFTER the run via exchange_instance("stats") — no
// per-frame wire output is needed (VAR was removed from core), so the script just
// drives the instances and lets their internal counters do the measuring.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& s = xi::use("serial");     // concurrency_probe    (not reentrant)
    auto& p = xi::use("parallel");   // concurrency_probe_rt (reentrant, uncapped)
    auto& c = xi::use("capped");     // concurrency_probe_rt (reentrant, max_concurrency=1)
    s.process(xi::Record().set("frame", frame));
    p.process(xi::Record().set("frame", frame));
    c.process(xi::Record().set("frame", frame));
}
