// Camera template — a raw source emitter (via xi::Emitter). On exchange
// {"frame":N} it stages one frame and emits it, tagging the dataInfo with the
// frame number N so a downstream collector can correlate the two cameras.
// Instantiated twice (cam_left, cam_right).
#include <xi/xi.hpp>
#include <xi/xi_emitter.hpp>
#include <cJSON.h>
#include <string>
class Camera : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    std::string exchange(const std::string& cmd) override {
        int frame = 0;
        if (cJSON* j = cJSON_Parse(cmd.c_str())) {
            cJSON* f = cJSON_GetObjectItem(j, "frame");
            if (cJSON_IsNumber(f)) frame = (int)f->valuedouble;
            cJSON_Delete(j);
        }
        em_.bind(host(), name());
        em_.image("img", pool_image(2, 2, 1));
        em_.emit("{\"frame\":" + std::to_string(frame) + "}");  // dataInfo: {"seq":N,"frame":F}
        return "{}";
    }
private:
    xi::Emitter em_;
};
XI_PLUGIN_IMPL(Camera)
