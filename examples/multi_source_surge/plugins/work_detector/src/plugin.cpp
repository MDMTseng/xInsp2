// work_detector.cpp — configurable-cost downstream detector for multi_source_surge.
//
// process(input) reads {seq, src_tag} stamped in the first 16 bytes of
// the input image, sleeps `sleep_ms` (config-bound, may be overridden
// per-call by input["sleep_ms"]), increments a per-src_tag atomic
// counter, and returns:
//
//   { seq, src_tag, processed_total, processed_for_src }
//
// MUST be reentrant. Two instances of this plugin (detector_fast,
// detector_slow) are subscribed to overlapping source streams and the
// inspection script runs under N=8 dispatch threads. Only state is
// std::atomic counters under a small mutex (the per-tag map is
// initialised lazily; per-tag reads use atomic loads).

#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

class WorkDetector : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        int sleep_ms = input["sleep_ms"].as_int(default_sleep_ms_.load());
        if (sleep_ms < 0)   sleep_ms = 0;
        if (sleep_ms > 200) sleep_ms = 200;

        uint64_t seq     = 0;
        uint64_t src_tag = 0;
        const auto& img = input.get_image("img");
        if (!img.empty() && img.data() != nullptr) {
            std::memcpy(&seq,     img.data(),     sizeof(seq));
            std::memcpy(&src_tag, img.data() + 8, sizeof(src_tag));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));

        int total = ++processed_total_;
        int for_src;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for_src = ++by_src_[src_tag];
        }

        return xi::Record()
            .set("seq",                (int)(seq & 0x7fffffff))
            .set("src_tag_lo",         (int)(src_tag & 0x7fffffff))
            .set("processed_total",    total)
            .set("processed_for_src",  for_src);
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("sleep_ms",        default_sleep_ms_.load())
            .set("processed_total", processed_total_.load())
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        int v = p["sleep_ms"].as_int(default_sleep_ms_.load());
        if (v < 0)   v = 0;
        if (v > 200) v = 200;
        default_sleep_ms_.store(v);
        return true;
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "reset") {
            processed_total_.store(0);
            std::lock_guard<std::mutex> lk(mu_);
            by_src_.clear();
        }
        return get_def();
    }

private:
    std::atomic<int> default_sleep_ms_{10};
    std::atomic<int> processed_total_{0};
    std::mutex                                  mu_;
    std::unordered_map<uint64_t, int>           by_src_;
};

XI_PLUGIN_IMPL(WorkDetector)
