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

    xi::Record process(const xi::Record& /*input*/) override {
        int cur = ++in_flight_;
        // Track the max concurrency seen (lock-free CAS climb).
        int prev = max_c_.load();
        while (cur > prev && !max_c_.compare_exchange_weak(prev, cur)) { /* retry */ }
        if (cur > 1) overlaps_.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));  // widen the window
        --in_flight_;
        return xi::Record()
            .set("maxc",     max_c_.load())
            .set("overlaps", overlaps_.load());
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("maxc",     max_c_.load())
            .set("overlaps", overlaps_.load())
            .dump();
    }
    bool set_def(const std::string& /*j*/) override { return true; }

private:
    std::atomic<int> in_flight_{0};
    std::atomic<int> max_c_{0};
    std::atomic<int> overlaps_{0};
};

XI_PLUGIN_IMPL(ConcurrencyProbe)
