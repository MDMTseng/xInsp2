//
// synced_stereo.cpp — synthetic stereo camera: a GATHERING source that grabs
// both cameras and emits left+right in ONE record. Multi-camera sync needs no
// bus policy — the frames are correlated because they ride the same record.
//
// Per tick:
//   1. Build two distinct frames (left = vertical stripes, right = horizontal)
//      stamped with the same `seq` so the script can verify they really come
//      from the same event.
//   2. emit(Record().image("left", L).image("right", R))  // the RAII source path
//
// The dispatched event carries both images; the script reads them via
// xi::current_trigger().image("left") and .image("right").
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_thread.hpp>   // xi::spawn_worker — SEH-safe capture thread

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
        // Blessed source thread: xi::spawn_worker installs the per-thread SEH
        // translator + top-level catch so a stray fault in the grab loop is
        // contained to this worker instead of taking down the whole backend.
        thread_ = xi::spawn_worker(name() + "-source", [this] { run_loop_(); });
    }

    void stop_() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    std::atomic<int> seq_{0};

    void emit_one_() {
        const int W = 320, H = 240;
        int seq = seq_.fetch_add(1);

        // LEFT: vertical stripes. RIGHT: horizontal stripes. Same seq. Paint
        // straight into fresh host-pool slots so emit() hands them over via the
        // pool refcount path (no heap-to-pool copy).
        xi::Image L = pool_image(W, H, 1);
        xi::Image R = pool_image(W, H, 1);
        uint8_t* lp = L.data();
        uint8_t* rp = R.data();
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                lp[y * W + x] = (uint8_t)(((x + seq) & 31) ? 200 : 32);
                rp[y * W + x] = (uint8_t)(((y + seq) & 31) ? 32 : 200);
            }
        // Stamp seq into the top-left 4 bytes of each so the script can verify
        // "these came from the same event".
        std::memcpy(lp, &seq, sizeof(seq));
        std::memcpy(rp, &seq, sizeof(seq));

        // Both frames in ONE record → one dispatched event, no bus policy.
        // emit() fills host()/name() and runs the RAII marshal/refcount path.
        xi::Record rec = xi::Record()
            .image("left",  L)
            .image("right", R);
        emit(rec);
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
