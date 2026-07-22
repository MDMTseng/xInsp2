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
#include <xi/xi_script_pack.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    // Each probe ignores its input; a minimal pack carrying "frame" drives the
    // xi.pack@1 door so the host's per-instance dispatch lock is exercised.
    auto mk = [&] { xi::ScriptPackBuilder b; b.add_i64("frame", frame); return b.seal(); };
    xi::use("serial").process(mk());     // concurrency_probe    (not reentrant)
    xi::use("parallel").process(mk());   // concurrency_probe_rt (reentrant, uncapped)
    xi::use("capped").process(mk());     // concurrency_probe_rt (reentrant, max_concurrency=1)
}
