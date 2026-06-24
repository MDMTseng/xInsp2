//
// config_swap_probe.cpp — reference plugin for the orchestrator config-swap
// design (docs/roadmap/config-bundles-and-orchestration.md).
//
// Demonstrates the DOUBLE-SLOT (prepare/commit) pattern a "heavy resource"
// plugin uses so its config can be swapped frame-perfectly without stalling the
// pipeline while assets load:
//
//   * `active_`  — the LIVE resource that process() reads. Lock-free
//                  std::atomic<shared_ptr>; a concurrent reader always sees a
//                  fully-built resource (old XOR new), never a torn one.
//   * `staged_`  — the BACKGROUND slot. prepare() builds the (simulated heavy)
//                  resource here WITHOUT touching `active_`, so the running
//                  pipeline keeps using the old config during the load.
//   * commit()   — a single atomic pointer swap: active_ = staged_.
//
// Two ways the host drives it (both via this one plugin):
//   * `set_instance_def` → set_def(): the TIER-1 immediate path — load + swap in
//     one step. The host serializes this vs process() for a non-reentrant plugin,
//     so it's safe with zero plugin-side locking, just a brief stall while loading.
//   * `prepare_instance` → prepare() then a host `commit_group` → commit(): the
//     FRAME-PERFECT path. prepare() stages in the background (host calls it UNGATED,
//     concurrent with process — we touch ONLY staged_), then the host drains
//     dispatch and fires commit() across a whole group in one no-process window,
//     so no inspection run ever sees a half-committed group.
//
// prepare()/commit() are first-class ABI v7 verbs (task #69): the plugin opts in
// with XI_PLUGIN_STAGED below. The `folder` arg is accepted but the probe loads
// from an inline `value` instead of real files — a production plugin would read
// its heavy assets from that folder.
//
// Build: part of plugins/CMakeLists.txt (target config_swap_probe).
//

#include <xi/xi_abi.hpp>   // xi::Plugin, xi::Record, XI_PLUGIN_IMPL

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>

// A stand-in for a heavy loaded asset (model weights, sbm template, calibration).
// The fill + checksum loop in the constructor simulates a non-trivial load cost,
// so building one is the "slow" part that prepare() keeps off the live path.
struct Resource {
    int                value = 0;
    long long          checksum = 0;
    std::vector<int>   asset;
    explicit Resource(int v) : value(v), asset(1u << 16, v) {
        long long acc = 0;
        for (int x : asset) acc += x;   // pretend this is expensive asset prep
        checksum = acc;
    }
};

// Minimal field reader — the probe's "def"/commands are tiny flat JSON objects
// like {"value":42} / {"command":"prepare","value":42}. Mirrors the lightweight
// string parsing the other example plugins use; no DOM needed.
static int parse_int_field(const std::string& j, const char* key, int dflt) {
    std::string k = std::string("\"") + key + "\":";
    auto pos = j.find(k);
    if (pos == std::string::npos) return dflt;
    return std::atoi(j.c_str() + pos + k.size());
}

class ConfigSwapProbe : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // TIER-1 immediate path: load + swap in one step. Host serializes vs process()
    // for a non-reentrant plugin, so no plugin-side lock is needed.
    bool set_def(const std::string& j) override {
        active_.store(std::make_shared<const Resource>(parse_int_field(j, "value", 0)));
        return true;
    }

    std::string get_def() const override {
        auto a = active_.load();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{\"value\":%d}", a ? a->value : 0);
        return buf;
    }

    // Lock-free read of the LIVE slot. A swap that happens concurrently (commit on
    // another thread) is invisible mid-run: this run sees old XOR new, never torn.
    xi::Record process(const xi::Record&) override {
        auto a = active_.load();
        last_seen_.store(a ? a->value : -1);
        proc_calls_.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    // ABI v7 first-class STAGE: build the heavy resource into the STAGING slot.
    // The host calls this UNGATED (concurrent with process()), so we touch ONLY
    // staged_ — never active_. The live slot keeps serving the old config during
    // the (here simulated) heavy load. `folder` would be where a production plugin
    // reads its real assets; this probe loads from an inline value instead.
    bool prepare(const std::string& def, const std::string& /*folder*/) override {
        staged_.store(std::make_shared<const Resource>(parse_int_field(def, "value", 0)));
        return true;
    }

    // ABI v7 first-class COMMIT: atomic pointer swap of staged → live. Cheap.
    // Under a host commit_group, dispatch is already drained, so it's uncontended.
    void commit() override {
        if (auto s = staged_.load()) { active_.store(s); staged_.store(nullptr); }
    }

    std::string exchange(const std::string& cmd) override {
        if (cmd.find("\"get_status\"") != std::string::npos) {
            return status_json();
        }
        return R"({"error":"unknown command"})";
    }

private:
    std::atomic<std::shared_ptr<const Resource>> active_{std::make_shared<const Resource>(0)};
    std::atomic<std::shared_ptr<const Resource>> staged_{nullptr};
    std::atomic<int>                             last_seen_{-1};
    std::atomic<long long>                       proc_calls_{0};

    std::string status_json() const {
        auto a = active_.load();
        auto s = staged_.load();
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"active\":%d,\"staged\":%s,\"staged_value\":%d,\"last_seen\":%d,\"proc\":%lld}",
            a ? a->value : 0,
            s ? "true" : "false",
            s ? s->value : -1,
            last_seen_.load(),
            (long long)proc_calls_.load());
        return buf;
    }
};

XI_PLUGIN_IMPL(ConfigSwapProbe)
XI_PLUGIN_STAGED(ConfigSwapProbe)   // opt into ungated prepare() + commit()
