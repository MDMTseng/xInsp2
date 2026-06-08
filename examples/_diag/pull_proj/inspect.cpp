// Pull-model smoke consumer: pull the frame the "src" emitter staged under
// res_id "latest" and expose what came back as vars. Mirrors how a real script
// would read a frame by id instead of via core-correlated current_trigger().
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
XI_SCRIPT_EXPORT void xi_inspect_entry(int) {
    auto r = xi::use("src").fetch("latest");
    VAR(ok, r.ok() ? 1 : 0);
    VAR(data, r.data());               // dataInfo cJSON, e.g. {"seq":N}
    auto img = r.image("img");         // lazy pull by key
    VAR(w, img.width);
    VAR(h, img.height);
}
