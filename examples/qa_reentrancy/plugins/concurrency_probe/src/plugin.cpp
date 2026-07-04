//
// concurrency_probe.cpp — measures how many dispatch workers are inside this
// instance's process() at once. Used by examples/qa_reentrancy to prove the
// declared-reentrancy safety model for the parallel dispatch pool:
//
//   * a NON-reentrant plugin (this folder, no "reentrant" in plugin.json) must
//     be serialized per-instance by the host -> max concurrency observed == 1,
//     even with parallelism.dispatch_threads > 1;
//   * a REENTRANT plugin (the sibling concurrency_probe_rt, "reentrant": true)
//     opts out of that lock -> the host runs its process() truly in parallel,
//     so max concurrency observed climbs above 1.
//
// The counters are atomic on purpose: the probe must observe concurrency
// without itself racing. A short sleep widens the window so overlapping workers
// actually coincide.
//
#include <xi/xi.hpp>
#include <xi/xi_json.hpp>
#include <atomic>
#include <chrono>
#include <thread>

class ConcurrencyProbe : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    void process(xi::PackIn& /*in*/, xi::PackOut& out) override {
        calls_.fetch_add(1);
        int cur = ++in_flight_;
        // Track the max concurrency seen (lock-free CAS climb).
        int prev = max_c_.load();
        while (cur > prev && !max_c_.compare_exchange_weak(prev, cur)) { /* retry */ }
        if (cur > 1) overlaps_.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));  // widen the window
        --in_flight_;
        out.i64("maxc",     max_c_.load())
           .i64("overlaps", overlaps_.load());
    }

    // The driver reads the accumulated counters here AFTER the run — no per-frame
    // wire output needed (the counters live in the instance across process() calls).
    //   "stats" (default) -> { maxc, overlaps, calls }
    //   "reset"           -> zero the counters
    std::string exchange(const std::string& cmd) override {
        if (cmd.find("reset") != std::string::npos) {
            max_c_.store(0); overlaps_.store(0); calls_.store(0);
            return "{\"ok\":true}";
        }
        return stats_json();
    }

    std::string get_def() const override { return stats_json(); }
    bool set_def(const std::string& /*j*/) override { return true; }

private:
    std::string stats_json() const {
        return xi::Json::object()
            .set("maxc",     max_c_.load())
            .set("overlaps", overlaps_.load())
            .set("calls",    calls_.load())
            .dump();
    }
    std::atomic<int> in_flight_{0};
    std::atomic<int> max_c_{0};
    std::atomic<int> overlaps_{0};
    std::atomic<int> calls_{0};
};

XI_PLUGIN_IMPL(ConcurrencyProbe)
XI_PLUGIN_PACK_DOOR(ConcurrencyProbe)
