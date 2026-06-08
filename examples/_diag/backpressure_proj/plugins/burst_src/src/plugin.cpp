// Phase B-2 back-pressure proof: one exchange bursts 200 emit_dispatch calls in
// a tight loop. With a tiny lane (queue_depth=2) and the worker doing a real
// (slower) inspect per run, the producer outpaces the consumer, so the lane
// fills and emit_dispatch returns 0 (REJECT, not silent drop). Reports the
// accepted/rejected split so the driver can assert back-pressure engaged.
#include <xi/xi.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
class BurstSrc : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    std::string exchange(const std::string& /*cmd*/) override {
        int acc = 0, rej = 0;
        for (int i = 0; i < 200; ++i) {
            xi_trigger_id tid{0, (uint64_t)(++seq_)};
            char idbuf[40];
            std::snprintf(idbuf, sizeof(idbuf), "%016llx%016llx",
                          (unsigned long long)tid.hi, (unsigned long long)tid.lo);
            xi::Image img = pool_image(1, 1, 1);
            xi_record_image rec{}; rec.key = "img"; rec.handle = img.pool_handle();
            if (host() && host()->emit_resource)
                host()->emit_resource(name().c_str(), idbuf, &rec, 1, "{}");
            int ok = (host() && host()->emit_dispatch)
                       ? host()->emit_dispatch(name().c_str(), tid, 0) : 0;
            (ok ? acc : rej)++;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{\"accepted\":%d,\"rejected\":%d}", acc, rej);
        return buf;
    }
private:
    long long seq_ = 0;
};
XI_PLUGIN_IMPL(BurstSrc)
