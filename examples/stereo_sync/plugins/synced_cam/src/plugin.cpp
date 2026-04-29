//
// synced_cam.cpp — synthetic single-image source plugin.
//
// Two instances (cam_left, cam_right) emit independently to the framework's
// TriggerBus. Both derive their tid deterministically from the running
// sequence number so the bus's AllRequired policy can correlate them.
//
// Per-tick output:
//   - 320x240 grayscale frame
//   - First 4 bytes = little-endian uint32 sequence number (the script
//     reads these out and verifies left.seq == right.seq).
//
// The TID convention:
//   tid.hi = STEREO_TID_HI = 0x73796E635F636166ull  ("sync_cam" ASCII)
//   tid.lo = (uint64_t)seq + 1
// Both instances use the same formula keyed by their own seq counter, so
// frame N from cam_left and frame N from cam_right share a tid.
//
// Build: handled by the backend's project-plugin compile path (cl.exe).
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {
constexpr uint64_t STEREO_TID_HI = 0x73796E635F636166ull;  // "sync_caf"
}

class SyncedCam : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    SyncedCam(const xi_host_api* host, const char* name)
        : xi::Plugin(host, name)
    {
        // Start the worker as soon as the plugin instance is created.
        start_();
    }

    ~SyncedCam() override { stop_(); }

    std::string get_def() const override {
        return xi::Json::object()
            .set("running", running_.load())
            .set("fps", fps_.load())
            .set("seq", (int)seq_.load())
            .set("emitted", (int)emitted_.load())
            .set("width", 320)
            .set("height", 240)
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        int v = p["fps"].as_int(fps_.load());
        if (v < 1) v = 1;
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
            if (v < 1) v = 1;
            if (v > 240) v = 240;
            fps_.store(v);
        }
        return get_def();
    }

private:
    std::atomic<bool>     running_{false};
    std::atomic<int>      fps_{20};
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> emitted_{0};
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
        while (running_.load()) {
            uint32_t seq = seq_.fetch_add(1);

            xi_image_handle h = host_->image_create(W, H, 1);
            if (h == XI_IMAGE_NULL) {
                // out of pool; skip this tick
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1000 / std::max(fps_.load(), 1)));
                continue;
            }
            uint8_t* px = host_->image_data(h);
            // Fill pattern: flat midgrey, instance-distinct stripe, then stamp seq.
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    px[y * W + x] = (uint8_t)(((x + (int)seq) & 31) ? 200 : 32);
                }
            }
            // Stamp seq as little-endian uint32 in the first 4 bytes.
            std::memcpy(px, &seq, sizeof(seq));

            xi_record_image entry = { "img", h };
            xi_trigger_id tid;
            tid.hi = STEREO_TID_HI;
            tid.lo = (uint64_t)seq + 1;     // avoid (0,0) which is NULL

            host_->emit_trigger(name().c_str(), tid, /*ts=*/0, &entry, 1);
            host_->image_release(h);
            emitted_.fetch_add(1);

            int sleep_ms = 1000 / std::max(fps_.load(), 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }
};

XI_PLUGIN_IMPL(SyncedCam)
