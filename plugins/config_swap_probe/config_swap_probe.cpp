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

#include <xi/xi_abi.hpp>       // xi::Plugin, xi::PackIn/PackOut, XI_PLUGIN_IMPL
#include <xi/xi_json.hpp>      // canonical config/command parsing (over cmd.find/atoi)
#include <xi/xi_contract.hpp>  // schema-version stamp key + skew rejection

#include "config_swap_probe_keys.gen.h"  // guard 1: config/command/status key names, once

#include <atomic>
#include <cstring>             // std::strcmp — the pack-door get_interface trampoline
#include <memory>
#include <string>
#include <vector>

// Guard 1: the plugin's own readers compile from the SAME constants the typed
// view (config_swap_probe_io.h) builds from, so a key rename can't drift.
namespace ckeys = xi::config_swap_probe::keys;

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

// The probe's "def"/commands are tiny flat JSON objects like {"value":42} /
// {"command":"get_status"}. Read the resource value through the canonical parser
// so the key name comes from config_swap_probe_keys.h, not a literal (guard 1).
static int parse_value(const std::string& j, int dflt) {
    return xi::Json::parse(j)[ckeys::kValue].as_int(dflt);
}

// Guard 3: a config/def built against an incompatible header schema is rejected.
// A matching or absent stamp proceeds (legacy persisted defs carry no stamp).
static bool schema_ok(const std::string& j) {
    auto sv = xi::Json::parse(j)[xi::contract::kSchemaKey];
    return !sv.is_number() || sv.as_int() == xi::config_swap_probe::kSchemaVersion;
}

class ConfigSwapProbe : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // TIER-1 immediate path: load + swap in one step. Host serializes vs process()
    // for a non-reentrant plugin, so no plugin-side lock is needed.
    bool set_def(const std::string& j) override {
        if (!schema_ok(j)) {
            log_error("config_swap_probe: config schema mismatch (built for a "
                      "different header version than this plugin serves)");
            return false;
        }
        active_.store(std::make_shared<const Resource>(parse_value(j, 0)));
        return true;
    }

    std::string get_def() const override {
        auto a = active_.load();
        return xi::Json::object().set(ckeys::kValue, a ? a->value : 0).dump();
    }

    // polaris2 wave-2 (docs/new_gen/10 gate P1): the xi.pack@1 pack-in/pack-out
    // door — THE data plane (v12: the Record process path is deleted). This
    // probe's process() is a no-op OBSERVATION step: read the live slot into
    // last_seen_, bump the call counter, and return an EMPTY pack (the door's
    // absence sentinel). The probe emits NO output data; its real surface is the
    // config/prepare/commit control plane observed through get_status (below).
    // It has no input contract, so there are no required-input faults and
    // unknown pack entries are ignored.
    void process(xi::PackIn& /*in*/, xi::PackOut& /*out*/) override {
        auto a = active_.load();
        last_seen_.store(a ? a->value : -1);
        proc_calls_.fetch_add(1, std::memory_order_relaxed);
    }

    // ABI v7 first-class STAGE: build the heavy resource into the STAGING slot.
    // The host calls this UNGATED (concurrent with process()), so we touch ONLY
    // staged_ — never active_. The live slot keeps serving the old config during
    // the (here simulated) heavy load. `folder` would be where a production plugin
    // reads its real assets; this probe loads from an inline value instead.
    bool prepare(const std::string& def, const std::string& /*folder*/) override {
        if (!schema_ok(def)) {
            log_error("config_swap_probe: prepare schema mismatch");
            return false;
        }
        staged_.store(std::make_shared<const Resource>(parse_value(def, 0)));
        return true;
    }

    // ABI v7 first-class COMMIT: atomic pointer swap of staged → live. Cheap.
    // Under a host commit_group, dispatch is already drained, so it's uncontended.
    void commit() override {
        if (auto s = staged_.load()) { active_.store(s); staged_.store(nullptr); }
    }

    std::string exchange(const std::string& cmd) override {
        const std::string command = xi::Json::parse(cmd)[ckeys::kCommand].as_string();
        if (command == ckeys::kGetStatus) return status_json();
        return exchange_unknown_command(command);
    }

private:
    std::atomic<std::shared_ptr<const Resource>> active_{std::make_shared<const Resource>(0)};
    std::atomic<std::shared_ptr<const Resource>> staged_{nullptr};
    std::atomic<int>                             last_seen_{-1};
    std::atomic<long long>                       proc_calls_{0};

    std::string status_json() const {
        auto a = active_.load();
        auto s = staged_.load();
        return xi::Json::object()
            .set(ckeys::kActive,      a ? a->value : 0)
            .set(ckeys::kStaged,      s ? true : false)
            .set(ckeys::kStagedValue, s ? s->value : -1)
            .set(ckeys::kLastSeen,    last_seen_.load())
            .set(ckeys::kProc,        (long long)proc_calls_.load())
            .dump();
    }
};

XI_PLUGIN_IMPL(ConfigSwapProbe)
XI_PLUGIN_STAGED(ConfigSwapProbe)   // opt into ungated prepare() + commit()
// polaris2 wave-2: publish the xi.pack@1 pack-in/pack-out door (gate P1). The
// host probes xi_plugin_get_interface("xi.pack", 1) to learn the probe speaks
// packs; the Record path above is untouched — this instance speaks BOTH.
XI_PLUGIN_PACK_DOOR(ConfigSwapProbe)
