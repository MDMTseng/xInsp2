//
// synced_cam.cpp — synthetic stereo camera: a GATHERING source. ONE instance
// grabs BOTH cameras each tick and emits left+right in ONE record. The two
// frames are correlated because they ride the same record — no shared wall
// clock and no bus `all_required` policy are needed any more.
//
// Per tick:
//   - Build two distinct 320x240 frames (cam_left = vertical stripes,
//     cam_right = horizontal stripes), stamp the SAME LE u32 `seq` into the
//     first 4 bytes of each, and emit ONE record:
//       Record().image("cam_left", L).image("cam_right", R)
//   - The script decodes the embedded seqs and confirms left_seq == right_seq.
//
// TID convention (kept from the old two-instance model):
//   tid.hi = TID_HI_TAG = 0xC0FFEE
//   tid.lo = (uint64_t)seq
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
constexpr int      W          = 320;
constexpr int      H          = 240;
constexpr uint64_t TID_HI_TAG = 0xC0FFEEULL;   // distinguishes our TIDs
} // namespace

class SyncedCam : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    ~SyncedCam() override { stop_(); }

    std::string get_def() const override {
        return xi::Json::object()
            .set("running",    running_.load())
            .set("period_ms",  period_ms_)
            .set("auto_start", auto_start_)
            .set("ticks",      (int)ticks_.load())
            .set("last_seq",   (int)last_seq_.load())
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        period_ms_  = p["period_ms"].as_int(period_ms_);
        if (period_ms_ < 10)   period_ms_ = 10;
        if (period_ms_ > 1000) period_ms_ = 1000;
        auto_start_ = p["auto_start"].as_bool(auto_start_);
        // set_def() runs after the host has wired up the instance and the
        // host_api is fully usable, so this is the safe place to kick the
        // worker off.
        if (auto_start_ && !running_.load()) start_();
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
        else if (command == "set_period_ms") {
            int v = p["value"].as_int(period_ms_);
            if (v < 10)   v = 10;
            if (v > 1000) v = 1000;
            period_ms_ = v;
        }
        return get_def();
    }

private:
    std::atomic<bool>     running_{false};
    std::atomic<int>      ticks_{0};
    std::atomic<int>      last_seq_{-1};
    std::atomic<uint32_t> seq_{0};
    int                   period_ms_  = 50;
    bool                  auto_start_ = true;
    std::thread           thread_;

    void start_() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { run_loop_(); });
    }

    void stop_() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    void emit_one_() {
        uint32_t seq = seq_.fetch_add(1);

        // cam_left: vertical stripes. cam_right: horizontal stripes. Same seq.
        std::vector<uint8_t> L((size_t)W * H), R((size_t)W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                L[y * W + x] = (uint8_t)(((x + (int)seq) & 31) ? 200 : 32);
                R[y * W + x] = (uint8_t)(((y + (int)seq) & 31) ? 32 : 200);
            }
        // Stamp the LE uint32 seq into the first 4 bytes of each frame.
        std::memcpy(L.data(), &seq, sizeof(seq));
        std::memcpy(R.data(), &seq, sizeof(seq));

        // Both frames in ONE record → one dispatched event, no bus policy.
        xi_trigger_id tid{};
        tid.hi = TID_HI_TAG;
        tid.lo = (uint64_t)seq;

        xi::Record rec = xi::Record()
            .image("cam_left",  xi::Image(W, H, 1, L.data()))
            .image("cam_right", xi::Image(W, H, 1, R.data()));
        xi::emit_record(host_, name().c_str(), rec, tid);

        ++ticks_;
        last_seq_ = (int)seq;
    }

    void run_loop_() {
        while (running_.load()) {
            emit_one_();
            const int period = period_ms_ <= 0 ? 50 : period_ms_;
            std::this_thread::sleep_for(std::chrono::milliseconds(period));
        }
    }
};

XI_PLUGIN_IMPL(SyncedCam)
