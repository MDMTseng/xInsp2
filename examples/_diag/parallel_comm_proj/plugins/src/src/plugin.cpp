// Burst emitter for the parallel-comm reorder test — now via xi::Emitter (the
// pull-model emitter helper) instead of hand-rolled emit_resource/emit_dispatch.
// On exchange {"n":N} it emits N frames; the Emitter mints res_id + a contiguous
// seq and stages {"seq":i} into each frame's dataInfo automatically. emit()
// returns false on back-pressure (here the queue is large, so all go through).
#include <xi/xi.hpp>
#include <xi/xi_emitter.hpp>
#include <cJSON.h>
#include <cstdio>
#include <string>
class Src : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    std::string exchange(const std::string& cmd) override {
        int n = 12;
        if (cJSON* j = cJSON_Parse(cmd.c_str())) {
            cJSON* nn = cJSON_GetObjectItem(j, "n");
            if (cJSON_IsNumber(nn)) n = (int)nn->valuedouble;
            cJSON_Delete(j);
        }
        em_.bind(host(), name());
        int emitted = 0;
        for (int i = 0; i < n; ++i) {
            em_.image("img", pool_image(1, 1, 1));   // temporary — Emitter addrefs it
            if (em_.emit()) ++emitted;               // seq auto: 0,1,2,...
        }
        char buf[48];
        std::snprintf(buf, sizeof(buf), "{\"emitted\":%d}", emitted);
        return buf;
    }
private:
    xi::Emitter em_;
};
XI_PLUGIN_IMPL(Src)
