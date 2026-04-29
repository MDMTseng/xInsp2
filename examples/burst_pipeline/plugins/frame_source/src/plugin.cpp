// frame_source.cpp — synthetic image source for burst_pipeline.
//
// One worker thread per instance. Ticks at `fps` Hz using
// std::this_thread::sleep_until so the cumulative drift stays bounded
// even when the scheduler hiccups. Each tick:
//   - allocates a 320x240 grayscale pool image
//   - stamps the running uint64 sequence number into the first 8 bytes
//     (little-endian; on x86-64 we just memcpy)
//   - calls host->emit_trigger with a tid keyed off the seq
//   - releases the local handle
//
// No sleep inside process(); the heavy-work simulation lives in the
// inspect script (where dispatch_threads can actually parallelise it).
//
// isolation: must be "in_process" — emit_trigger only crosses the
// process boundary during in-flight RPCs and a pure source never has
// one. See docs/reference/instance-model.md.

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {
constexpr uint64_t BURST_TID_HI = 0x6275727374706970ull;  // "burstpip"
}

class FrameSource : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    FrameSource(const xi_host_api* host, const char* name)
        : xi::Plugin(host, name)
    {
        start_();
    }

    ~FrameSource() override { stop_(); }

    std::string get_def() const override {
        return xi::Json::object()
            .set("running",  running_.load())
            .set("fps",      fps_.load())
            .set("seq",      (int)(seq_.load() & 0x7fffffff))
            .set("emitted",  (int)(emitted_.load() & 0x7fffffff))
            .set("width",    320)
            .set("height",   240)
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        int v = p["fps"].as_int(fps_.load());
        if (v < 1)   v = 1;
        if (v > 240) v = 240;
        fps_.store(v);
        return true;
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "start") start_();
        else if (command == "stop") stop_();
        else if (command == "set_fps") {
            int v = p["value"].as_int(fps_.load());
            if (v < 1)   v = 1;
            if (v > 240) v = 240;
            fps_.store(v);
        }
        return get_def();
    }

private:
    std::atomic<bool>     running_{false};
    std::atomic<int>      fps_{60};
    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> emitted_{0};
    std::thread           thread_;

    void start_() {
        if (running_.load()) return;
        running_ = true;
        thread_ = std::thread([this] { run_loop_(); });
    }

    void stop_() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    void run_loop_() {
        const int W = 320, H = 240;
        using clk = std::chrono::steady_clock;
        auto next = clk::now();
        while (running_.load()) {
            uint64_t seq = seq_.fetch_add(1);

            xi_image_handle h = host_->image_create(W, H, 1);
            if (h != XI_IMAGE_NULL) {
                uint8_t* px = host_->image_data(h);
                // Cheap fill: midgrey background + a moving stripe so
                // visual debuggers can sanity-check the stream. Keep
                // this fast — the source is supposed to be ~free.
                std::memset(px, 200, (size_t)W * H);
                int stripe_x = (int)(seq % (uint64_t)W);
                for (int y = 0; y < H; ++y) {
                    px[y * W + stripe_x] = 32;
                }
                // Stamp seq as little-endian uint64 in first 8 bytes.
                std::memcpy(px, &seq, sizeof(seq));

                xi_record_image entry = { "img", h };
                xi_trigger_id   tid;
                tid.hi = BURST_TID_HI;
                tid.lo = seq + 1;            // avoid (0,0) which is NULL

                // timestamp_us = 0 → host stamps with its now() clock.
                // We need this for end-to-end latency measurement.
                host_->emit_trigger(name().c_str(), tid, /*ts=*/0,
                                    &entry, 1);
                host_->image_release(h);
                emitted_.fetch_add(1);
            }

            // sleep_until — cumulative drift stays bounded.
            int fps = fps_.load();
            if (fps < 1) fps = 1;
            auto period = std::chrono::microseconds(1'000'000 / fps);
            next += period;
            auto now = clk::now();
            if (next < now) {
                // We fell behind the schedule (slow scheduler tick or
                // image_create stall). Reset baseline so we don't busy-
                // spin emitting a backlog of ticks.
                next = now + period;
            }
            std::this_thread::sleep_until(next);
        }
    }
};

XI_PLUGIN_IMPL(FrameSource)
