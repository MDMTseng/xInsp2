// Collector template — a COMBINING emitter (the 乙-i multi-cam correlator),
// built on xi::Emitter. The script feeds it each raw camera frame's id+frame#
// (op:accept); when both cameras have reported the same frame number, the
// collector emits ONE correlated frame (its own Emitter assigns a fresh seq),
// carrying the two source res_ids so the inspection can pull either camera's
// pixels lazily. Correlation lives here, in a plugin — not in the core.
//
// Non-reentrant by default, so the host serializes accept() across lane workers;
// pending_ needs no extra lock.
#include <xi/xi.hpp>
#include <xi/xi_emitter.hpp>
#include <cJSON.h>
#include <map>
#include <cstdio>
#include <string>
class Collector : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    std::string exchange(const std::string& cmd) override {
        cJSON* root = cJSON_Parse(cmd.c_str());
        if (!root) return "{}";
        if (jstr(root, "op") == "accept") {
            std::string cam = jstr(root, "cam"), rid = jstr(root, "res_id");
            cJSON* fj = cJSON_GetObjectItem(root, "frame");
            int frame = (fj && cJSON_IsNumber(fj)) ? (int)fj->valuedouble : 0;
            auto& slot = pending_[frame];
            slot[cam] = rid;
            if (slot.count("cam_left") && slot.count("cam_right")) {   // pair complete
                em_.bind(host(), name());
                std::string meta = "{\"frame\":" + std::to_string(frame)
                                 + ",\"left\":\""  + slot["cam_left"]  + "\""
                                 + ",\"right\":\"" + slot["cam_right"] + "\"}";
                em_.emit(meta);                 // no images: correlate ids, pull pixels lazily
                pending_.erase(frame);
            }
        }
        cJSON_Delete(root);
        return "{}";
    }
private:
    static std::string jstr(cJSON* o, const char* k) {
        cJSON* v = cJSON_GetObjectItem(o, k);
        return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
    }
    xi::Emitter em_;
    std::map<int, std::map<std::string, std::string>> pending_;   // frame -> {cam -> res_id}
};
XI_PLUGIN_IMPL(Collector)
