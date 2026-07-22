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
#include <xi/xi_script_pack.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    // Drive the counter instance through its pack door (pack in, pack out).
    xi::ScriptPackBuilder cb;
    cb.add_i64("frame", frame);
    auto out = xi::use("counter").process(cb.seal());
    const long long count = out.get_i64("count").value_or(-1);

    // Surface count + the script marker through `expose` (channel "qa").
    xi::ScriptPackBuilder b;
    b.add_i64("count", count);
    b.add_i64("script", 1);      // default script marker (alt_inspect.cpp emits 2)
    b.add_i64("frame", frame);
    b.add_str("$channel", "qa");
    xi::use("expose").push(b.seal());
}
