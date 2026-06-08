#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
XI_SCRIPT_EXPORT void xi_inspect_entry(int f) {
    auto& c = xi::use("crasher");
    auto out = c.process(xi::Record().set("frame", f));
    VAR(count, out["count"].as_int(-1));
}
