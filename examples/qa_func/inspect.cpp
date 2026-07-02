//
// inspect.cpp — qa_func default script (FUNCTIONAL / lifecycle viewpoint).
//
// The "counter" instance (raw_thread_crash, armed:false) just returns a
// running count and never crashes, so the backend stays healthy under
// autostart. Output is surfaced via the `expose` plugin on channel "qa": a
// consumer (the driver) reads the values back through expose `get` / the XEX1
// push. Two values per frame:
//   count  — the instance's running frame count (advances in continuous mode)
//   script — a literal tag so AS-I3 can prove WHICH script was compiled
//            (this default vs the --script override alt_inspect.cpp).
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& counter = xi::use("counter");
    auto out = counter.process(xi::Record().set("frame", frame));
    xi::Record rec;
    rec.set("count", out["count"].as_int(-1))
       .set("script", 1)      // default script marker (alt_inspect.cpp emits 2)
       .set("frame", frame);
    rec.set("$channel", "qa");
    xi::use("expose").process(rec);
}
