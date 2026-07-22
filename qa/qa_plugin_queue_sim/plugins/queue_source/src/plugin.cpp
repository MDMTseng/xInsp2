// queue_source.cpp — a source plugin that OWNS its own work queue and paces its
// emits off the core lane's back-pressure. The exemplar for docs/new_gen/24
// ("plugin owns the queue"): the plugin holds a deep std::deque of pending work
// and releases it one item at a time via emit(); the core lane keeps only a
// minimal residual (queue_depth 0 = rendezvous, or 1 = pipeline), so the plugin's
// OWN deque is the only real backlog.
//
// This is the SAFE target for rendezvous/block back-pressure: it emits on a
// DEDICATED thread it can freely stall. It is NOT a worker of the lane it feeds and
// NOT a camera callback — parking it on a full/handoff slot is benign (it just
// paces to the drain), and a stop/teardown wakes it to drop.
//
// Starts IDLE. exchange "start" fills the internal deque with `backlog` items,
// resets counters, and spins the emit thread; it pops one item and emit()s it,
// then (for a depth-0 lane) blocks in the rendezvous until a worker TAKES it, or
// (for a depth-1 lane) returns as soon as the 1 slot frees so it can prepare/emit
// the next while the worker still computes the current — the pipelining.
//
// It records the wall-clock time each emit() RETURNS. The gap between the 1st and
// 2nd returns is the observable that separates the two rungs:
//   * depth=1 (pipeline): the 2nd emit returns almost immediately (the worker took
//     frame 0 and freed the slot, so frame 1 is deposited while frame 0 is still
//     being computed) → first_gap_ms ≈ 0.
//   * depth=0 (rendezvous): the 2nd emit can't return until frame 1 is TAKEN, which
//     only happens once the worker finishes computing frame 0 → first_gap_ms ≈ the
//     inspect time. Strict lock-step; no run-ahead.
// isolation must be "in_process".
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace { constexpr uint64_t QSRC_TID_HI = 0x71756575657372ull; }  // "queuesr"

class QueueSource : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    QueueSource(const xi_host_api* host, const char* name) : xi::Plugin(host, name) {}
    ~QueueSource() override { stop_(); }

    std::string get_def() const override {
        return xi::Json::object()
            .set("running",      running_.load())
            .set("backlog",      backlog_.load())          // how deep the PLUGIN's own queue is
            .set("emitted",      (int)(emitted_.load() & 0x7fffffff))
            .set("first_gap_ms", first_gap_ms_.load())     // gap between emit[0] and emit[1] returns
            .set("min_gap_ms",   min_gap_ms_.load())       // smallest inter-emit-return gap seen
            .set("fast_returns", fast_returns_.load())     // # emits (after the 1st) that returned "fast"
            .dump();
    }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        int b = p["backlog"].as_int(backlog_.load()); if (b < 1) b = 1;
        backlog_.store(b);
        int f = p["fast_ms"].as_int(fast_ms_.load()); if (f < 1) f = 1;
        fast_ms_.store(f);
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
    std::atomic<int>      backlog_{500};       // plugin-owned queue depth
    std::atomic<int>      fast_ms_{15};        // "fast" return threshold (< inspect time)
    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> emitted_{0};
    std::atomic<int>      first_gap_ms_{-1};
    std::atomic<int>      min_gap_ms_{-1};
    std::atomic<int>      fast_returns_{0};
    std::deque<uint64_t>  pending_;            // THE plugin-owned backlog (its own queue)
    std::mutex            pending_mu_;
    std::thread           thread_;

    void start_() {
        if (running_.load()) return;
        seq_.store(0); emitted_.store(0);
        first_gap_ms_.store(-1); min_gap_ms_.store(-1); fast_returns_.store(0);
        { std::lock_guard<std::mutex> lk(pending_mu_);
          pending_.clear();
          int n = backlog_.load();
          for (int i = 0; i < n; ++i) pending_.push_back((uint64_t)i); }
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
        xi_trigger_id tid; tid.hi = QSRC_TID_HI; tid.lo = seq + 1;
        emit(std::move(out), tid);                     // BLOCKS here on a rendezvous/full lane
        emitted_.fetch_add(1);
    }

    void run_loop_() {
        using clk = std::chrono::steady_clock;
        clk::time_point prev{};
        bool have_prev = false;
        // Pace is ENTIRELY the core lane's back-pressure: no sleep. The plugin just
        // drains its own deque as fast as the handoff/pipeline slot allows, which is
        // exactly "the plugin owns the queue, the core paces it".
        while (running_.load()) {
            uint64_t item;
            { std::lock_guard<std::mutex> lk(pending_mu_);
              if (pending_.empty()) break;             // backlog drained → done
              item = pending_.front(); pending_.pop_front(); }
            (void)item;
            emit_one_();                               // returns when handed off / slot free
            auto now = clk::now();
            if (have_prev) {
                int gap = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - prev).count();
                int fm = fast_ms_.load();
                // First recorded gap is between emit[0] and emit[1] — the run-ahead tell.
                if (first_gap_ms_.load() < 0) first_gap_ms_.store(gap);
                int mn = min_gap_ms_.load();
                if (mn < 0 || gap < mn) min_gap_ms_.store(gap);
                if (gap < fm) fast_returns_.fetch_add(1);
            }
            prev = now; have_prev = true;
        }
        running_ = false;
    }
};

XI_PLUGIN_IMPL(QueueSource)
