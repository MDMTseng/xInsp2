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
#include <yyjson.h>
#include <map>
#include <cstdio>
#include <string>
class Collector : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    std::string exchange(const std::string& cmd) override {
        yyjson_doc* doc = yyjson_read(cmd.c_str(), cmd.size(), 0);
        yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (!root) { yyjson_doc_free(doc); return "{}"; }
        if (jstr(root, "op") == "accept") {
            std::string cam = jstr(root, "cam"), rid = jstr(root, "res_id");
            yyjson_val* fj = yyjson_obj_get(root, "frame");
            int frame = (fj && yyjson_is_num(fj)) ? (int)yyjson_get_num(fj) : 0;
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
        yyjson_doc_free(doc);
        return "{}";
    }
private:
    static std::string jstr(yyjson_val* o, const char* k) {
        yyjson_val* v = yyjson_obj_get(o, k);
        return (v && yyjson_is_str(v) && yyjson_get_str(v)) ? yyjson_get_str(v) : "";
    }
    xi::Emitter em_;
    std::map<int, std::map<std::string, std::string>> pending_;   // frame -> {cam -> res_id}
};
XI_PLUGIN_IMPL(Collector)
