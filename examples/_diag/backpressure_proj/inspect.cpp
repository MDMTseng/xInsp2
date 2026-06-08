#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
XI_SCRIPT_EXPORT void xi_inspect_entry(int) {
    auto t = xi::current_trigger();
    auto r = xi::use("src").get(t.id_string());
    VAR(ok, r.ok() ? 1 : 0);
}
