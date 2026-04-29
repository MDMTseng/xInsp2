// burst_source.cpp — heterogeneous synthetic image source for multi_source_surge.
//
// One worker thread per instance. Behaviour controlled by `shape` (config-set):
//
//   "steady"   — emits at exactly fps Hz, sleep_until-cadenced
//   "bursty"   — idles between bursts; an external driver triggers a burst
//                via exchange({command:"burst", count:N}); the worker then
//                fires N frames back-to-back with no inter-frame sleep,
//                then returns to idle (low background fps = 2 Hz "heartbeat")
//   "variable" — fps varies sinusoidally between 0.5x and 1.5x of nominal
//                with a 1-second period; produces variable inter-arrival
//                times that should still average to nominal fps
//
// Each frame's pixel buffer carries:
//   bytes [0..7]   uint64 LE seq            (per-instance running counter)
//   bytes [8..15]  uint64 LE src_tag        (FNV-1a hash of instance name)
//   bytes [16..]   midgrey + moving stripe (visual debug; cheap)
//
// src_tag lets the inspect script attribute every frame to its source
// instance without a string compare in the hot path. The script doesn't
// know instance names at compile time; it learns them at runtime via
// xi::current_trigger().sources() and computes the same FNV-1a hash to
// match against this stamp.
//
// isolation: "in_process" (sources never own an in-flight RPC).

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {
constexpr uint64_t MSS_TID_HI = 0x6d73735f74726967ull;  // "mss_trig"

// FNV-1a 64-bit. Same hash recomputed in inspect.cpp; keep in sync.
constexpr uint64_t fnv1a64(const char* s) {
    uint64_t h = 0xcbf29ce484222325ull;
    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= 0x100000001b3ull;
    }
    return h;
}
} // namespace

class BurstSource : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    BurstSource(const xi_host_api* host, const char* name)
        : xi::Plugin(host, name)
    {
        src_tag_ = fnv1a64(name);
        start_();
    }

    ~BurstSource() override { stop_(); }

    std::string get_def() const override {
        return xi::Json::object()
            .set("running",  running_.load())
            .set("fps",      fps_.load())
            .set("width",    width_.load())
            .set("height",   height_.load())
            .set("shape",    shape_str_())
            .set("seq",      (int)(seq_.load() & 0x7fffffff))
            .set("emitted",  (int)(emitted_.load() & 0x7fffffff))
            .set("burst_pending", burst_pending_.load())
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        int v = p["fps"].as_int(fps_.load());
        if (v < 1)   v = 1;
        if (v > 240) v = 240;
        fps_.store(v);
        int w = p["width"].as_int(width_.load());
        int h = p["height"].as_int(height_.load());
        if (w >= 32 && w <= 1920) width_.store(w);
        if (h >= 32 && h <= 1080) height_.store(h);
        auto sh = p["shape"].as_string();
        if (!sh.empty()) set_shape_from_string_(sh);
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
        } else if (command == "burst") {
            int n = p["count"].as_int(0);
            if (n < 0)    n = 0;
            if (n > 1000) n = 1000;
            burst_pending_.fetch_add(n);
        }
        return get_def();
    }

private:
    enum Shape { STEADY = 0, BURSTY = 1, VARIABLE = 2 };

    std::atomic<bool>     running_{false};
    std::atomic<int>      fps_{30};
    std::atomic<int>      width_{320};
    std::atomic<int>      height_{240};
    std::atomic<int>      shape_{STEADY};
    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> emitted_{0};
    std::atomic<int>      burst_pending_{0};
    uint64_t              src_tag_{0};
    std::thread           thread_;

    std::string shape_str_() const {
        switch (shape_.load()) {
            case BURSTY:   return "bursty";
            case VARIABLE: return "variable";
            default:       return "steady";
        }
    }

    void set_shape_from_string_(const std::string& s) {
        if      (s == "bursty")   shape_.store(BURSTY);
        else if (s == "variable") shape_.store(VARIABLE);
        else                      shape_.store(STEADY);
    }

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
        int W = width_.load();
        int H = height_.load();
        if (W < 32 || H < 32) return;

        uint64_t seq = seq_.fetch_add(1);
        xi_image_handle h = host_->image_create(W, H, 1);
        if (h == XI_IMAGE_NULL) return;

        uint8_t* px = host_->image_data(h);
        std::memset(px, 200, (size_t)W * H);
        // Stripe (cheap visual sanity).
        int stripe_x = (int)(seq % (uint64_t)W);
        for (int y = 0; y < H; ++y) px[y * W + stripe_x] = 32;
        // Stamp seq + src_tag.
        std::memcpy(px,     &seq,      sizeof(seq));
        std::memcpy(px + 8, &src_tag_, sizeof(src_tag_));

        xi_record_image entry = { "img", h };
        xi_trigger_id   tid;
        tid.hi = MSS_TID_HI ^ src_tag_;     // separate keyspace per source
        tid.lo = seq + 1;                   // avoid (0,0)

        host_->emit_trigger(name().c_str(), tid, /*ts=*/0, &entry, 1);
        host_->image_release(h);
        emitted_.fetch_add(1);
    }

    void run_loop_() {
        using clk = std::chrono::steady_clock;
        auto next = clk::now();
        auto t0 = clk::now();

        while (running_.load()) {
            // Drain any pending burst — fires count frames back-to-back.
            int pending = burst_pending_.exchange(0);
            for (int i = 0; i < pending && running_.load(); ++i) {
                emit_one_();
            }

            int  shape = shape_.load();
            int  fps   = fps_.load();
            if (fps < 1) fps = 1;

            if (shape == BURSTY) {
                // Background heartbeat ~2 Hz when no burst is queued so
                // the source still shows up in dispatch_stats.
                emit_one_();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // Steady / variable both do one emit per loop iteration with
            // a sleep_until cadence.
            emit_one_();

            int64_t period_us = 1'000'000 / fps;
            if (shape == VARIABLE) {
                // Sinusoidal jitter: scale period by 0.5..1.5 over a
                // 1-second period. Average period stays at 1/fps.
                auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
                              clk::now() - t0).count();
                double phase = (double)(dt % 1'000'000) / 1'000'000.0;
                double mult  = 1.0 + 0.5 * std::sin(phase * 6.2831853);
                if (mult < 0.2) mult = 0.2;
                period_us = (int64_t)(period_us * mult);
            }

            next += std::chrono::microseconds(period_us);
            auto now = clk::now();
            if (next < now) next = now + std::chrono::microseconds(period_us);
            std::this_thread::sleep_until(next);
        }
    }
};

XI_PLUGIN_IMPL(BurstSource)
