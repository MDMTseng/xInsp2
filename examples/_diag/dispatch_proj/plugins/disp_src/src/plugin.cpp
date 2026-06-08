// Phase-B smoke source: per exchange it mints a fresh id, STAGES a frame under
// that id's hex string (emit_resource), then SIGNALS a run for it
// (emit_dispatch) — bypassing the trigger bus entirely. The script reads the id
// back via current_trigger().id_string() and pulls the frame by it.
#include <xi/xi.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
class DispSrc : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    std::string exchange(const std::string& /*cmd*/) override {
        xi_trigger_id tid{0, (uint64_t)(++seq_)};
        char idbuf[40];
        std::snprintf(idbuf, sizeof(idbuf), "%016llx%016llx",
                      (unsigned long long)tid.hi, (unsigned long long)tid.lo);
        xi::Image img = pool_image(5, 2, 1);
        if (auto* d = img.data()) d[0] = 7;
        xi_record_image rec{}; rec.key = "img"; rec.handle = img.pool_handle();
        std::string cjson = "{\"seq\":" + std::to_string(seq_) + "}";
        if (host()) {
            if (host()->emit_resource)
                host()->emit_resource(name().c_str(), idbuf, &rec, 1, cjson.c_str());
            if (host()->emit_dispatch)
                host()->emit_dispatch(name().c_str(), tid, 0);
        }
        return "{}";
    }
private:
    long long seq_ = 0;
};
XI_PLUGIN_IMPL(DispSrc)
