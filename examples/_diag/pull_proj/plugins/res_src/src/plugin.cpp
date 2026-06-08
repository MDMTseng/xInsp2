// Pull-model smoke source: on any exchange it STAGES one frame (a 4x3x1 image
// keyed "img" + cJSON metadata carrying a monotonic seq) under the fixed res_id
// "latest" via host->emit_resource. No trigger — the driver drives one inspect()
// with cmd:run, and the script pulls "latest" back via xi::use("src").get().
// Proves emit_resource -> ResourceStore -> get_resource/get_resource_image.
#include <xi/xi.hpp>
#include <string>
class ResSrc : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    std::string exchange(const std::string& /*cmd*/) override {
        xi::Image img = pool_image(4, 3, 1);
        if (auto* d = img.data()) d[0] = 42;   // mark a pixel so width/height/data are real
        xi_record_image rec{};
        rec.key = "img";
        rec.handle = img.pool_handle();
        std::string cjson = "{\"seq\":" + std::to_string(++seq_) + "}";
        if (host() && host()->emit_resource)
            host()->emit_resource(name().c_str(), "latest", &rec, 1, cjson.c_str());
        return "{}";
    }
private:
    int seq_ = 0;
};
XI_PLUGIN_IMPL(ResSrc)
