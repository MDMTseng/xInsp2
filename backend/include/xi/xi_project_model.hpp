#pragma once
//
// xi_project_model.hpp — the project DATA MODEL: the plain-old-data structs that
// describe an opened project (instances, trigger policy, parallelism / dispatch
// groups, compile environment). Extracted from xi_plugin_manager.hpp so the
// model is separable from the manager that loads/mutates it. No behaviour here,
// just shapes + a tiny find_group helper.
//
// (NOTE: distinct from xi_project.hpp, which is a thin set of project.json
// read/write helpers in namespace xi::project — not the model.)
//
#include "xi_instance.hpp"      // InstanceBase (held by InstanceInfo)
#include "xi_trigger_bus.hpp"   // TriggerPolicy

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace xi {

struct InstanceInfo {
    std::string          name;
    std::string          plugin_name;
    int                  max_concurrency = 0;  // instance.json cap; 0 = unlimited (reentrant)
    std::string          folder_path;  // project/instances/<name>/
    std::string          group;        // instance.json "group" → dispatch group for this source's triggers ("" = default)
    std::shared_ptr<InstanceBase> instance;
};

struct ProjectInfo {
    std::string name;
    std::string folder_path;
    std::string script_path;
    std::vector<std::string> plugin_names;  // which plugins are used
    std::unordered_map<std::string, InstanceInfo> instances;

    // Trigger bus policy persisted per project. Default is Any (every
    // source emit fires one dispatch) — back-compat with pre-trigger
    // plugins. Change to AllRequired for multi-camera synchronisation.
    TriggerPolicy trigger_policy    = TriggerPolicy::Any;
    std::vector<std::string> trigger_required;    // source names (AllRequired)
    std::string   trigger_leader;                 // source name (LeaderFollowers)
    int           trigger_window_ms = 100;

    // `parallelism.dispatch_threads`: how many dispatcher threads run
    // xi_inspect_entry concurrently in continuous mode. Default 1 (single
    // threaded). With no explicit `groups`, this becomes the synthesized default
    // dispatch lane's max_parallel (see spawn_group_pool_). Caveats (xi::state,
    // plugin process() reentrance, watchdog) — see writing-a-script.md.
    int           dispatch_threads = 1;

    // `parallelism.queue_depth`: maximum number of trigger events (real bus emits
    // + synthetic timer ticks) buffered while workers are busy. Default 100.
    int           queue_depth      = 100;

    // `parallelism.overflow`: what to do when queue_depth is full and another
    // event arrives. "drop_oldest" (default; live prefers freshest) /
    // "drop_newest" (keep FIFO; archival) / "block" (back-pressure the source).
    std::string   overflow         = "drop_oldest";

    // `parallelism.result_order`: how per-frame results (vars + previews +
    // run_finished) are ordered on the wire under N>1. "completion" (default;
    // emit as each worker finishes) / "arrival" (emit in frame-arrival order via
    // the lane's emit gate). Only meaningful when there's real concurrency.
    std::string   result_order     = "completion";

    // ---- Dispatch groups (priority + concurrency lanes) --------------------
    // Optional. Each group owns its own worker threads (`max_parallel`) at its OS
    // `thread_priority` + its own queue; a trigger is routed by the emitting
    // source instance's "group". When ABSENT, a single default lane is
    // synthesized from the parallelism fields above — dispatch is one unified
    // lane model either way. See docs/design/dispatch-groups.md.
    struct DispatchGroup {
        std::string name;
        int         max_parallel    = 1;          // worker threads this group owns
        std::string thread_priority = "normal";   // "high" | "normal" | "low" (OS)
        int         queue_depth     = 100;
        std::string overflow        = "drop_oldest";
        std::string result_order    = "completion"; // "completion" | "arrival" (per-group)
        int         min_interval_ms = 0;             // 0 = unlimited; else min spacing between dispatch starts (rate cap)
        // CPU affinity (OS). Empty = unbound (default — OS schedules freely). Each
        // inner vector is a core-set MASK (a worker may run on ANY of those cores,
        // not pinned to one). Sizes: 0 = unbound; 1 = all workers share that mask;
        // N = worker i uses set[i % N]. Parsed from "cpu_affinity":
        //   [0,1,2,3]      → one shared mask {0,1,2,3}
        //   [[0,1],[2,3]]  → per-worker masks
        std::vector<std::vector<int>> cpu_affinity;
    };
    std::vector<DispatchGroup> groups;            // empty = one synthesized default lane
    std::string                default_group;     // group for untagged triggers

    // project.json "runtime" block — operational knobs that are also live-settable
    // (cmd:set_process_priority / set_timer_fps). Applied by the backend on open.
    std::string                runtime_priority;  // "" = leave OS priority as-is
    int                        runtime_timer_fps = -1;   // <0 = unset; 0 = trigger-only; >0 = fps

    const DispatchGroup* find_group(const std::string& n) const {
        for (auto& g : groups) if (g.name == n) return &g;
        return nullptr;
    }
};

// Static compile environment for project-level plugins. Populated once at
// backend startup so PluginManager doesn't need to know about service-main
// globals; mirrors the fields service_main feeds into xi::script::compile
// for inspection scripts.
struct CompileEnv {
    std::string include_dir;
    std::string vcvars_path;
    std::string opencv_dir;
    std::string turbojpeg_root;
    std::string ipp_root;
    bool        aot = false;   // AOT bundle: load prebuilt plugin DLLs, don't compile
};

} // namespace xi
