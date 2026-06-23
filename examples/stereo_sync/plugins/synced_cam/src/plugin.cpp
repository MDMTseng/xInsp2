//
// synced_cam.cpp — synthetic stereo camera: a GATHERING source. ONE instance
// grabs BOTH cameras each tick and emits left+right in ONE record. Multi-camera
// sync needs no bus policy — the frames are correlated because they ride the
// same record.
//
// Per tick:
//   1. Build two distinct 320x240 frames (cam_left = vertical stripes,
//      cam_right = horizontal stripes) stamped with the SAME `seq` in the first
//      4 bytes so the script can verify left_seq == right_seq.
//   2. xi::emit_record(host, name, Record().image("cam_left", L)
//                                          .image("cam_right", R), tid)
//
// The dispatched event carries both images keyed by the record's own keys, so
// the script reads them via xi::current_trigger().image("cam_left") and
// .image("cam_right").
//
// The TID convention (kept from the old two-instance model):
//   tid.hi = STEREO_TID_HI = 0x73796E635F636166ull  ("sync_caf")
//   tid.lo = (uint64_t)seq + 1   (avoid (0,0) which is NULL)
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {
constexpr uint64_t STEREO_TID_HI = 0x73796E635F636166ull;  // "sync_caf"
}

class SyncedCam : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    SyncedCam(const xi_host_api* host, const char* name) : xi::Plugin(host, name) {
        start_();   // a camera runs on its own — emit as soon as the instance exists
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
        if      (command == "start") start_();
        else if (command == "stop")  stop_();
        else if (command == "fire") {                 // deterministic headless drive
            int n = p["n"].as_int(1); if (n < 1) n = 1; if (n > 10000) n = 10000;
            for (int i = 0; i < n; ++i) emit_one_();
        }
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

    void emit_one_() {
        const int W = 320, H = 240;
        uint32_t seq = seq_.fetch_add(1);

        // cam_left: vertical stripes. cam_right: horizontal stripes. Same seq.
        std::vector<uint8_t> L((size_t)W * H), R((size_t)W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                L[y * W + x] = (uint8_t)(((x + (int)seq) & 31) ? 200 : 32);
                R[y * W + x] = (uint8_t)(((y + (int)seq) & 31) ? 32 : 200);
            }
        // Stamp seq as little-endian uint32 in the first 4 bytes of each so the
        // script can verify "these came from the same event".
        std::memcpy(L.data(), &seq, sizeof(seq));
        std::memcpy(R.data(), &seq, sizeof(seq));

        // Both frames in ONE record → one dispatched event, no bus policy.
        xi_trigger_id tid;
        tid.hi = STEREO_TID_HI;
        tid.lo = (uint64_t)seq + 1;     // avoid (0,0) which is NULL

        xi::Record rec = xi::Record()
            .image("cam_left",  xi::Image(W, H, 1, L.data()))
            .image("cam_right", xi::Image(W, H, 1, R.data()));
        xi::emit_record(host_, name().c_str(), rec, tid);
        emitted_.fetch_add(1);
    }

    void run_loop_() {
        while (running_.load()) {
            emit_one_();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1000 / std::max(fps_.load(), 1)));
        }
    }
};

XI_PLUGIN_IMPL(SyncedCam)
