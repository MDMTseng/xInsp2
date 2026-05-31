//
// concurrency_probe_rt.cpp — identical to concurrency_probe, but its plugin.json
// declares "reentrant": true. See concurrency_probe/src/plugin.cpp for the full
// explanation. This variant opts out of the host's per-instance lock, so under a
// parallel dispatch pool its process() runs truly concurrently and the max
// concurrency it observes climbs above 1.
//
#include <xi/xi.hpp>
#include <xi/xi_json.hpp>
#include <atomic>
#include <chrono>
#include <thread>

class ConcurrencyProbeRt : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& /*input*/) override {
        int cur = ++in_flight_;
        int prev = max_c_.load();
        while (cur > prev && !max_c_.compare_exchange_weak(prev, cur)) { /* retry */ }
        if (cur > 1) overlaps_.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        --in_flight_;
        return xi::Record()
            .set("maxc",     max_c_.load())
            .set("overlaps", overlaps_.load());
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("maxc",     max_c_.load())
            .set("overlaps", overlaps_.load())
            .dump();
    }
    bool set_def(const std::string& /*j*/) override { return true; }

private:
    std::atomic<int> in_flight_{0};
    std::atomic<int> max_c_{0};
    std::atomic<int> overlaps_{0};
};

XI_PLUGIN_IMPL(ConcurrencyProbeRt)
