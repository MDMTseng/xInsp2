//
// synced_stereo.cpp — synthetic stereo camera: a GATHERING source that grabs
// both cameras and emits left+right in ONE record. Multi-camera sync needs no
// bus policy — the frames are correlated because they ride the same record.
//
// Per tick:
//   1. Build two distinct frames (left = vertical stripes, right = horizontal)
//      stamped with the same `seq` so the script can verify they really come
//      from the same event.
//   2. xi::emit_record(host, name, Record().image("left", L).image("right", R))
//
// The dispatched event carries both images; the script reads them via
// xi::current_trigger().image("left") and .image("right").
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

class SyncedStereo : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    ~SyncedStereo() override { stop_(); }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if      (command == "start") start_();
        else if (command == "stop")  stop_();
        else if (command == "fire") {                 // deterministic headless drive
            int n = p["n"].as_int(1); if (n < 1) n = 1; if (n > 10000) n = 10000;
            for (int i = 0; i < n; ++i) emit_one_();
        }
        else if (command == "set_fps") {
            int v = p["value"].as_int(fps_.load());
            if (v < 1) v = 1;
            if (v > 120) v = 120;
            fps_ = v;
        }
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("running", running_.load())
            .set("fps", fps_.load())
            .set("ticks", (int)ticks_.load())
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        fps_ = p["fps"].as_int(fps_.load());
        return true;
    }

private:
    std::atomic<bool> running_{false};
    std::atomic<int>  ticks_{0};
    // Written from the control thread (exchange/set_def), read by run_loop_()'s
    // worker — atomic for the same reason mock_camera's config fields are.
    std::atomic<int>  fps_{10};
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

    std::atomic<int> seq_{0};

    void emit_one_() {
        const int W = 320, H = 240;
        int seq = seq_.fetch_add(1);

        // LEFT: vertical stripes. RIGHT: horizontal stripes. Same seq.
        std::vector<uint8_t> L((size_t)W * H), R((size_t)W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                L[y * W + x] = (uint8_t)(((x + seq) & 31) ? 200 : 32);
                R[y * W + x] = (uint8_t)(((y + seq) & 31) ? 32 : 200);
            }
        // Stamp seq into the top-left 4 bytes of each so the script can verify
        // "these came from the same event".
        std::memcpy(L.data(), &seq, sizeof(seq));
        std::memcpy(R.data(), &seq, sizeof(seq));

        // Both frames in ONE record → one dispatched event, no bus policy.
        xi::Record rec = xi::Record()
            .image("left",  xi::Image(W, H, 1, L.data()))
            .image("right", xi::Image(W, H, 1, R.data()));
        xi::emit_record(host_, name().c_str(), rec, XI_TRIGGER_NULL);
        ticks_++;
    }

    void run_loop_() {
        while (running_.load()) {
            emit_one_();
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / std::max(fps_.load(), 1)));
        }
    }
};

XI_PLUGIN_IMPL(SyncedStereo)
