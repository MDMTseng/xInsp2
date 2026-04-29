//
// synced_cam.cpp — synthetic 320x240 grayscale camera. The plugin is
// designed to be instantiated TWICE in a project (cam_left, cam_right).
// Both instances emit on the trigger bus with TIDs derived from a shared
// wall-clock epoch, so the bus correlates them as a pair under the
// `all_required` policy.
//
// Tid coordination strategy
// -------------------------
// Each worker thread sleeps until the next period_ms boundary on the
// system_clock, computes seq = floor(now_ms / period_ms), and emits
// with tid = {hi=0xC0FFEE, lo=(uint64_t)seq}. Because seq depends only
// on the global wall clock (not on per-instance state) both cameras
// produce identical TIDs at each boundary, regardless of when each
// instance started.
//
// The seq is also memcpy'd into the first 4 bytes of the frame's pixel
// buffer (little-endian uint32), so the script can independently
// confirm correlation by comparing the embedded seqs.
//
// Isolation note
// --------------
// Per docs/reference/host_api.md, source plugins MUST run in_process
// because the worker stub emit_trigger is a no-op. Each instance.json
// for cam_left / cam_right sets "isolation": "in_process".
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {
constexpr int    W            = 320;
constexpr int    H            = 240;
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
        // set_def() runs after the host has wired up the instance and
        // the host_api is fully usable, so this is the safe place to
        // kick the worker off (vs. the ctor where emit_trigger would
        // race with the bus's set_sink).
        if (auto_start_ && !running_.load()) start_();
        return true;
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if      (command == "start") start_();
        else if (command == "stop")  stop_();
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

    static int64_t now_ms_wall_() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
    }

    void run_loop_() {
        while (running_.load()) {
            const int period = period_ms_ <= 0 ? 50 : period_ms_;
            int64_t now = now_ms_wall_();
            int64_t next_boundary = ((now / period) + 1) * period;
            // Sleep until the next boundary so both instances align.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(next_boundary - now));
            if (!running_.load()) break;

            int64_t boundary = now_ms_wall_();
            // Snap to the boundary we just woke at (handles slight wake
            // overshoot) so cam_left and cam_right agree on `seq`.
            int64_t seq = boundary / period;

            // Allocate the host pool image and write seq + a tiny pattern.
            xi_image_handle h = host_->image_create(W, H, 1);
            if (h == XI_IMAGE_NULL) continue;
            uint8_t* px = host_->image_data(h);
            // Fill pattern that depends on seq so the frame visually
            // changes per cycle (mid-grey by default).
            std::memset(px, (uint8_t)(50 + (seq & 127)), (size_t)W * H);
            // Stamp the LE uint32 seq into the first 4 bytes — always
            // overwrites the pattern there, which is fine.
            uint32_t seq32 = (uint32_t)(seq & 0xFFFFFFFFULL);
            std::memcpy(px, &seq32, sizeof(seq32));

            // Build the TID. Both cams produce the same value because
            // `seq` is derived from the global wall clock + a shared
            // period.
            xi_trigger_id tid{};
            tid.hi = TID_HI_TAG;
            tid.lo = (uint64_t)seq;

            xi_record_image entry{ "frame", h };
            // Pass timestamp_us=0 so the host stamps with its own clock —
            // we only care that TIDs match, not the per-instance ts.
            host_->emit_trigger(name().c_str(), tid, /*ts=*/0,
                                &entry, /*image_count=*/1);

            // Bus addref'd internally; release our ref.
            host_->image_release(h);

            ++ticks_;
            last_seq_ = (int)seq32;
        }
    }
};

XI_PLUGIN_IMPL(SyncedCam)
