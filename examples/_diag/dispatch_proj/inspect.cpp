// Phase-B smoke consumer: the run was driven by emit_dispatch (no bus). Read the
// dispatched id back and pull the frame the emitter staged under it.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
XI_SCRIPT_EXPORT void xi_inspect_entry(int) {
    auto t = xi::current_trigger();
    std::string res_id = t.id_string();
    auto r = xi::use("src").fetch(res_id);
    VAR(ok, r.ok() ? 1 : 0);
    VAR(data, r.data());
    auto img = r.image("img");
    VAR(w, img.width);
    VAR(h, img.height);
}
