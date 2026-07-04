// burst_source.cpp — paced image source for the overflow:"block" test.
//
// This is the SAFE target for overflow:block: it emits on a DEDICATED thread it
// can freely stall (a "back-pressure-tolerant" source). It is NOT a worker of the
// lane it feeds and NOT a camera callback — so parking it on a full block queue is
// benign (it just paces to the drain), and a stop/teardown wakes it to drop.
//
// Starts IDLE. exchange "start" resets seq/emitted counters and spins the emit
// thread, which emits ONE trigger every period_ms until "stop" (which joins it).
// Each trigger carries a 16x16 image with the running seq in the first 8 bytes.
// isolation must be "in_process".
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace { constexpr uint64_t BLOCK_TID_HI = 0x626c6f636b737263ull; }  // "blocksrc"

class BurstSource : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    // Starts IDLE (unlike the group-parallelism burst_source) so the test controls
    // exactly when emission begins — every emitted frame then lands AFTER cmd:start,
    // in the continuous lane, so emitted==processed is a clean lossless invariant.
    BurstSource(const xi_host_api* host, const char* name) : xi::Plugin(host, name) {}
    ~BurstSource() override { stop_(); }

    std::string get_def() const override {
        return xi::Json::object()
            .set("running",   running_.load())
            .set("period_ms", period_ms_.load())
            .set("emitted",   (int)(emitted_.load() & 0x7fffffff))
            .dump();
    }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        int t = p["period_ms"].as_int(period_ms_.load()); if (t < 1) t = 1;
        period_ms_.store(t);
        return true;
    }
    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "start") start_();
        else if (command == "stop") stop_();
        return get_def();
    }

private:
    std::atomic<bool>     running_{false};
    std::atomic<int>      period_ms_{15};
    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> emitted_{0};
    std::thread           thread_;

    void start_() {
        if (running_.load()) return;
        seq_.store(0); emitted_.store(0);   // clean per-run counters
        running_ = true;
        thread_ = std::thread([this] { run_loop_(); });
    }
    void stop_() { running_ = false; if (thread_.joinable()) thread_.join(); }

    void emit_one_() {
        const int W = 16, H = 16;
        uint64_t seq = seq_.fetch_add(1);
        std::vector<uint8_t> px((size_t)W * H, 128);
        std::memcpy(px.data(), &seq, sizeof(seq));     // seq in first 8 bytes
        xi::PackOut out = new_pack();
        out.image("img", W, H, 1, px.data());
        xi_trigger_id tid; tid.hi = BLOCK_TID_HI; tid.lo = seq + 1;
        emit(std::move(out), tid);
        emitted_.fetch_add(1);
    }

    void run_loop_() {
        using clk = std::chrono::steady_clock;
        // Steady paced emit: one trigger per period_ms. With period_ms well below the
        // inspect time, the source out-runs the drain and (on a block lane) parks on
        // enqueue until a slot frees — that IS the back-pressure under test.
        while (running_.load()) {
            auto t0 = clk::now();
            emit_one_();
            auto next = t0 + std::chrono::milliseconds(period_ms_.load());
            while (running_.load() && clk::now() < next)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

XI_PLUGIN_IMPL(BurstSource)
