//
// trigger_source.cpp — minimal image source using the TriggerBus API.
//
// Demonstrates the one piece of machinery the older ImageSource-style
// cameras can't do: stamping each emitted frame with a 128-bit trigger
// id so the host can CORRELATE multiple sources firing "at the same
// event" (hardware pulse, software trigger, external I/O, whatever).
//
// Single-source case: pass XI_TRIGGER_NULL and let the host allocate an
// id for you. That alone makes every frame individually addressable
// (replayable, recordable).
//
// Multi-source case: a pair of cameras trigger-synced in hardware share
// ONE tid across both emits. Under the AllRequired bus policy, the
// script only dispatches when both have emitted for that tid — no
// mis-pairing on dropped frames.
//
// To see correlation in action, wire two `trigger_source` instances
// under the same project and set trigger_policy → all_required. Each
// instance generates its own tid here; for real pairing swap the
// XI_TRIGGER_NULL below with a shared tid from your hardware.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

class TriggerSource : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    ~TriggerSource() override { stop_(); }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if      (command == "start") start_();
        else if (command == "stop")  stop_();
        else if (command == "set_fps")
            fps_ = std::max(1, std::min(120, p["value"].as_int(fps_)));
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("running", running_.load())
            .set("fps",     fps_)
            .set("ticks",   (int)ticks_.load())
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        fps_ = p["fps"].as_int(fps_);
        return true;
    }

private:
    std::atomic<bool> running_{false};
    std::atomic<int>  ticks_{0};
    int               fps_ = 10;
    std::thread       thread_;

    void start_() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { run_loop_(); });
    }

    void stop_() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    void run_loop_() {
        const int W = 320, H = 240;
        int seq = 0;
        while (running_.load()) {
            // 1. Create one frame in the host's image pool (refcount=1).
            xi_image_handle h = host_->image_create(W, H, 1);
            uint8_t* px = host_->image_data(h);
            for (int i = 0; i < W * H; ++i) px[i] = (uint8_t)((i + seq) & 0xFF);

            // 2. Emit it. XI_TRIGGER_NULL asks the host for a fresh tid
            //    (single-source mode); pass a shared tid to pair across
            //    multiple sources. timestamp_us=0 → host uses now().
            xi_record_image frame = { "frame", h };
            host_->emit_trigger(name().c_str(),
                                XI_TRIGGER_NULL,
                                /*timestamp_us=*/0,
                                &frame, 1);

            // 3. Bus addref'd the handle internally; release OURS so the
            //    pool can free it once the script is done reading.
            host_->image_release(h);

            ++seq;
            ticks_++;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1000 / std::max(fps_, 1)));
        }
    }
};

XI_PLUGIN_IMPL(TriggerSource)
