//
// inspect.cpp — qa_reentrancy. Each frame pokes BOTH probe instances:
//   * "serial"   -> concurrency_probe      (not reentrant -> host serializes)
//   * "parallel" -> concurrency_probe_rt   (reentrant -> host runs concurrent)
// Under parallelism.dispatch_threads=4 the driver reads the emitted maxc to
// prove the lock holds for the non-reentrant one and is absent for the other.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& s = xi::use("serial");     // concurrency_probe    (not reentrant)
    auto& p = xi::use("parallel");   // concurrency_probe_rt (reentrant, uncapped)
    auto& c = xi::use("capped");     // concurrency_probe_rt (reentrant, max_concurrency=1)
    auto rs = s.process(xi::Record().set("frame", frame));
    auto rp = p.process(xi::Record().set("frame", frame));
    auto rc = c.process(xi::Record().set("frame", frame));
    VAR(maxc_serial,     rs["maxc"].as_int(-1));
    VAR(overlaps_serial, rs["overlaps"].as_int(-1));
    VAR(maxc_parallel,   rp["maxc"].as_int(-1));
    VAR(maxc_capped,     rc["maxc"].as_int(-1));
}
