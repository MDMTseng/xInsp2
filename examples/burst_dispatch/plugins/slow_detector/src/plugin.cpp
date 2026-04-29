//
// slow_detector — process() sleeps for `sleep_ms` ms. That's it.
//
// Used by burst_dispatch to verify project.json parallelism.dispatch_threads
// actually parallelises long-running inspects. With 4 threads and 50 ms
// sleeps, 8 frames should land in ~100 ms wall-clock instead of ~400 ms.
//

#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <thread>

class SlowDetector : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        int sleep_ms = input["sleep_ms"].as_int(50);
        if (sleep_ms < 0) sleep_ms = 0;
        if (sleep_ms > 5000) sleep_ms = 5000;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        int n = ++count_;
        return xi::Record().set("count", n).set("slept_ms", sleep_ms);
    }

    std::string get_def() const override {
        return xi::Json::object().set("count", count_.load()).dump();
    }
    bool set_def(const std::string&) override { return true; }

private:
    std::atomic<int> count_{0};
};

XI_PLUGIN_IMPL(SlowDetector)
