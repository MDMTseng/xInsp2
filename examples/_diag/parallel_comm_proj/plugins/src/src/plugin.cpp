// Burst emitter for the parallel-comm reorder test: on exchange {"n":N} it
// stages N frames (emit_resource, each carrying {"seq":i}) and dispatches each
// (emit_dispatch) into the source's lane. With the lane at max_parallel=4 and a
// random sleep in the script, the frames COMPLETE out of order; the comm plugin
// must restore seq order downstream.
#include <xi/xi.hpp>
#include <cJSON.h>
#include <cstdint>
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
        int emitted = 0;
        for (int i = 0; i < n; ++i) {
            xi_trigger_id tid{0, (uint64_t)i};
            char idbuf[40];
            std::snprintf(idbuf, sizeof(idbuf), "%016llx%016llx",
                          (unsigned long long)tid.hi, (unsigned long long)tid.lo);
            xi::Image img = pool_image(1, 1, 1);
            xi_record_image rec{}; rec.key = "img"; rec.handle = img.pool_handle();
            std::string meta = "{\"seq\":" + std::to_string(i) + "}";
            if (host() && host()->emit_resource)
                host()->emit_resource(name().c_str(), idbuf, &rec, 1, meta.c_str());
            if (host() && host()->emit_dispatch
                && host()->emit_dispatch(name().c_str(), tid, 0)) ++emitted;
        }
        char buf[48];
        std::snprintf(buf, sizeof(buf), "{\"emitted\":%d}", emitted);
        return buf;
    }
};
XI_PLUGIN_IMPL(Src)
