// burst_source.cpp — bursty image source for the group-parallelism test.
//
// Every `period_ms` it emits `burst` triggers BACK-TO-BACK (an instantaneous
// surge), then idles until the next period. Set burst = 3x the target group's
// max_parallel so the group's queue fills and all its worker lanes engage at
// once — that's what makes the per-group max_parallel observable.
//
// Each trigger carries a tiny image with the running seq stamped in the first
// 8 bytes (same convention as frame_source). isolation must be "in_process".
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace { constexpr uint64_t BURST_TID_HI = 0x6275727374677270ull; }  // "burstgrp"

class BurstSource : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    BurstSource(const xi_host_api* host, const char* name) : xi::Plugin(host, name) { start_(); }
    ~BurstSource() override { stop_(); }

    std::string get_def() const override {
        return xi::Json::object()
            .set("running",   running_.load())
            .set("burst",     burst_.load())
            .set("period_ms", period_ms_.load())
            .set("emitted",   (int)(emitted_.load() & 0x7fffffff))
            .dump();
    }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        int b = p["burst"].as_int(burst_.load());       if (b < 1) b = 1;
        int t = p["period_ms"].as_int(period_ms_.load()); if (t < 10) t = 10;
        burst_.store(b); period_ms_.store(t);
        return true;
    }
    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "start") start_();
        else if (command == "stop") stop_();
        return get_def();
    }

private:
    std::atomic<bool>     running_{false};
    std::atomic<int>      burst_{3};
    std::atomic<int>      period_ms_{1000};
    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> emitted_{0};
    std::thread           thread_;

    void start_() { if (running_.load()) return; running_ = true; thread_ = std::thread([this] { run_loop_(); }); }
    void stop_()  { running_ = false; if (thread_.joinable()) thread_.join(); }

    void emit_one_() {
        const int W = 16, H = 16;
        xi_image_handle h = host_->image_create(W, H, 1);
        if (h == XI_IMAGE_NULL) return;
        uint64_t seq = seq_.fetch_add(1);
        uint8_t* px = host_->image_data(h);
        std::memset(px, 128, (size_t)W * H);
        std::memcpy(px, &seq, sizeof(seq));            // seq in first 8 bytes
        xi_record_image entry = { "img", h };
        xi_trigger_id tid; tid.hi = BURST_TID_HI; tid.lo = seq + 1;
        host_->emit_trigger(name().c_str(), tid, /*ts=*/0, &entry, 1);
        host_->image_release(h);
        emitted_.fetch_add(1);
    }

    void run_loop_() {
        using clk = std::chrono::steady_clock;
        while (running_.load()) {
            auto t0 = clk::now();
            const int burst = burst_.load();
            for (int i = 0; i < burst && running_.load(); ++i) emit_one_();   // instantaneous surge
            // Idle until the next period, in small chunks so stop is responsive.
            auto next = t0 + std::chrono::milliseconds(period_ms_.load());
            while (running_.load() && clk::now() < next)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

XI_PLUGIN_IMPL(BurstSource)
