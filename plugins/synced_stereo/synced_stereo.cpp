//
// synced_stereo.cpp — synthetic stereo camera that publishes left+right
// frames under ONE trigger ID. Demonstrates the emit_trigger correlation
// guarantee for multi-camera workflows.
//
// Per tick:
//   1. Generate a fresh tid (representing a hardware-pulse trigger event)
//   2. Build two distinct frames (left = stripes, right = solid+counter)
//      stamped with the same `seq` so the script can verify they really
//      come from the same event.
//   3. host->emit_trigger(name, tid, ts, [left, right])  ← single call
//
// The bus delivers both images under the same TriggerEvent. Script reads
// via xi::current_trigger().image("synced0/left") and .image("synced0/right").
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

class SyncedStereo : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    ~SyncedStereo() override { stop_(); }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if      (command == "start") start_();
        else if (command == "stop")  stop_();
        else if (command == "set_fps") {
            int v = p["value"].as_int(fps_);
            if (v < 1) v = 1;
            if (v > 120) v = 120;
            fps_ = v;
        }
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("running", running_.load())
            .set("fps", fps_)
            .set("ticks", (int)ticks_.load())
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
        int seq = 0;
        while (running_.load()) {
            // Build LEFT frame: vertical stripes + the seq number.
            xi_image_handle hL = host_->image_create(W, H, 1);
            uint8_t* pL = host_->image_data(hL);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    pL[y * W + x] = (uint8_t)(((x + seq) & 31) ? 200 : 32);

            // Build RIGHT frame: horizontal stripes + the seq number.
            xi_image_handle hR = host_->image_create(W, H, 1);
            uint8_t* pR = host_->image_data(hR);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    pR[y * W + x] = (uint8_t)(((y + seq) & 31) ? 32 : 200);

            // Stamp the seq into the top-left 4 bytes of each so the
            // script can verify "these came from the same event".
            std::memcpy(pL, &seq, sizeof(seq));
            std::memcpy(pR, &seq, sizeof(seq));

            // Emit BOTH frames atomically with one auto-generated tid.
            xi_record_image entries[2] = {
                { "left",  hL },
                { "right", hR },
            };
            host_->emit_trigger(name().c_str(), XI_TRIGGER_NULL, /*ts=*/0,
                                 entries, 2);

            // Bus addref'd both handles internally; release ours.
            host_->image_release(hL);
            host_->image_release(hR);

            ++seq;
            ticks_++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / std::max(fps_, 1)));
        }
    }
};

XI_PLUGIN_IMPL(SyncedStereo)
