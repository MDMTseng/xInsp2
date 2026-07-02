// xi_health.hpp — the core-owned health / state contract (schema xi.health/1).
//
// The coherence primitive four external reviews independently asked for (triage
// Bucket E / adoption-map item 10): ONE canonical read of the system's health,
// owned by core, subsuming the ad-hoc liveness side channels (get_state, status,
// dispatch_stats' last_emit_age, watchdog trips) under one small, versioned
// surface. Design: docs/new_gen/04-health-contract.md.
//
// This registry stores exactly THREE things — the top-level state, the script
// component's health, and a per-instance runtime-fault overlay (the quarantine
// seed) — because those are the only pieces of health truth no other core owner
// already holds. Instance base state (PluginManager's InstState), dispatch groups
// (the lanes), and source liveness (TriggerBus emit ages) are read from their
// existing owners when get_health is answered, so they cannot drift. Duplicating
// them here is the exact hand-synced-representation failure the v3 architecture
// exists to kill (and the concrete bug the instance-state map hit before it was
// moved into the PluginManager).
//
// Cost discipline: updates are an atomic state word + a mutex-guarded map touched
// ONLY on lifecycle / fault events — NEVER on the per-frame path. A per-frame
// verdict does not touch health; a caught fault (the rare exception path) does.
//
// Header-only + inline (like xi_metrics.hpp) so the state machine is unit-testable
// without linking the service. It has no dependency on the Engine — the derived
// components are assembled by the service layer (service_health.cpp), which owns
// g_eng access.
//
#ifndef XI_HEALTH_HPP
#define XI_HEALTH_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "xi_clock.hpp"   // xi::wall_ms

namespace xi {

// The schema version stamped on get_health + every health_changed event. Bumped
// only on a breaking shape change (additive fields do NOT bump it).
inline constexpr const char* kHealthSchema = "xi.health/1";

// Top-level process state. See docs/new_gen/04-health-contract.md for the legal
// transition table.
enum class SysState { Boot, ProjectLoaded, Running, Degraded, Draining, Fault };

// Per-component health.
enum class CompHealth { Ok, Degraded, Failed };

// Component kinds (wire strings).
inline constexpr const char* kKindScript   = "script";
inline constexpr const char* kKindInstance = "instance";
inline constexpr const char* kKindGroup    = "group";
inline constexpr const char* kKindSource   = "source";

// Reason codes (wire strings). Empty == no reason (health ok).
inline constexpr const char* kReasonPluginFault   = "plugin_fault";    // caught process()/exchange() crash → degraded
inline constexpr const char* kReasonPrepareFailed = "prepare_failed";  // prepare/commit/set_def failed → failed
inline constexpr const char* kReasonCompileError  = "compile_error";   // compile_and_load failed → failed (script)
inline constexpr const char* kReasonWatchdogTrip  = "watchdog_trip";   // fatal → fault
inline constexpr const char* kReasonQuarantined   = "quarantined";     // on_fault=refuse pulled it from service → failed (item 14)

inline const char* sys_state_name(SysState s) {
    switch (s) {
        case SysState::Boot:          return "boot";
        case SysState::ProjectLoaded: return "project_loaded";
        case SysState::Running:       return "running";
        case SysState::Degraded:      return "degraded";
        case SysState::Draining:      return "draining";
        case SysState::Fault:         return "fault";
    }
    return "boot";
}

inline const char* comp_health_name(CompHealth h) {
    switch (h) {
        case CompHealth::Ok:       return "ok";
        case CompHealth::Degraded: return "degraded";
        case CompHealth::Failed:   return "failed";
    }
    return "ok";
}

// Append `s` to `out` as a JSON string literal (quoted + escaped). Kept local so
// the header carries no dependency on xi_protocol's escaper (the unit test links
// only this header + yyjson).
inline void health_json_str(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[((unsigned char)c >> 4) & 0xF];
                    out += hex[(unsigned char)c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

class HealthRegistry {
public:
    // Called with a ready-to-send `health_changed` event JSON on any change.
    // Installed once at boot (service_main), before any lifecycle activity.
    using Notifier = std::function<void(const std::string& event_json)>;

    static HealthRegistry& instance() {
        static HealthRegistry h;
        return h;
    }

    void set_notifier(Notifier fn) {
        std::lock_guard<std::mutex> lk(mu_);
        notifier_ = std::move(fn);
    }

    // --- top-level state machine --------------------------------------------

    // Request a top-level state transition. Coalesces a no-op (same state).
    // Applies the transition even if undocumented (logging is the service's job
    // via the notifier) — the machine never wedges. Emits health_changed on any
    // real change. Returns true if the state changed.
    bool set_state(SysState s) {
        std::unique_lock<std::mutex> lk(mu_);
        if (s == state_) return false;
        state_ = s;
        state_since_ms_ = xi::wall_ms();
        emit_(lk, nullptr);
        return true;
    }

    SysState state() const {
        std::lock_guard<std::mutex> lk(mu_);
        return state_;
    }

    int64_t state_since_ms() const {
        std::lock_guard<std::mutex> lk(mu_);
        return state_since_ms_;
    }

    // --- script component ----------------------------------------------------

    // Record the script component's health (ok on a good compile+load, failed +
    // compile_error otherwise). Re-derives running/degraded when applicable.
    void set_script(CompHealth h, const std::string& reason, const std::string& name) {
        std::unique_lock<std::mutex> lk(mu_);
        if (script_present_ && script_health_ == h && script_reason_ == reason &&
            script_name_ == name)
            return;   // coalesce
        script_present_ = true;
        script_health_  = h;
        script_reason_  = reason;
        script_name_    = name;
        script_since_   = xi::wall_ms();
        recompute_locked_();
        Comp c{ kKindScript, script_name_, script_health_, script_reason_, script_since_ };
        emit_(lk, &c);
    }

    // The script was unloaded — drop it from the component set.
    void clear_script() {
        std::unique_lock<std::mutex> lk(mu_);
        if (!script_present_) return;
        script_present_ = false;
        script_health_  = CompHealth::Ok;
        script_reason_.clear();
        script_name_.clear();
        recompute_locked_();
        emit_(lk, nullptr);
    }

    // Read the script component for a get_health snapshot. Returns false when no
    // script is loaded.
    bool script_snapshot(std::string& name, CompHealth& health,
                         std::string& reason, int64_t& since_ms) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (!script_present_) return false;
        name = script_name_; health = script_health_;
        reason = script_reason_; since_ms = script_since_;
        return true;
    }

    // --- per-instance runtime-fault overlay ----------------------------------

    // Mark an instance's per-instance runtime-fault overlay (a caught
    // process()/exchange() crash, or an on_fault=refuse quarantine). This is the
    // ONLY per-instance health the registry stores — the quarantine seed
    // (adoption-map item 14). `health` is `Degraded` (kept in service — reuse /
    // reinit) or `Failed` (pulled from service — refuse/quarantine). Re-derives
    // running/degraded (any overlay entry, degraded OR failed, is a runtime
    // unhealthy input that keeps the top state `degraded`).
    void mark_instance_fault(const std::string& name, CompHealth health,
                             const std::string& reason) {
        std::unique_lock<std::mutex> lk(mu_);
        auto it = degraded_.find(name);
        if (it != degraded_.end() && it->second.health == health &&
            it->second.reason == reason)
            return;  // coalesce
        int64_t now = xi::wall_ms();
        degraded_[name] = DegradedInst{ health, reason, now };
        recompute_locked_();
        Comp c{ kKindInstance, name, health, reason, now };
        emit_(lk, &c);
    }

    // Back-compat convenience: a runtime crash marks the instance `degraded`.
    void mark_instance_degraded(const std::string& name, const std::string& reason) {
        mark_instance_fault(name, CompHealth::Degraded, reason);
    }

    // Clear an instance's runtime-fault overlay (re-activated / removed / project
    // closed). No-op (no event) if it wasn't degraded. Re-derives running/degraded
    // and emits a state-level event if the overlay actually changed.
    bool clear_instance_degraded(const std::string& name) {
        std::unique_lock<std::mutex> lk(mu_);
        if (degraded_.erase(name) == 0) return false;
        recompute_locked_();
        emit_(lk, nullptr);
        return true;
    }

    // Drop the whole overlay (project close). Emits once if anything was cleared.
    void clear_all_instance_degraded() {
        std::unique_lock<std::mutex> lk(mu_);
        if (degraded_.empty()) return;
        degraded_.clear();
        recompute_locked_();
        emit_(lk, nullptr);
    }

    // Look up an instance's runtime-fault overlay (health + reason) for the
    // get_health merge. Returns false when the instance carries no overlay.
    bool instance_fault(const std::string& name, CompHealth& health,
                        std::string& reason, int64_t& since_ms) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = degraded_.find(name);
        if (it == degraded_.end()) return false;
        health = it->second.health; reason = it->second.reason;
        since_ms = it->second.since_ms;
        return true;
    }

    // Back-compat: is there ANY runtime-fault overlay for `name`? (reason/since
    // out; health folded away — callers that need it use instance_fault.)
    bool instance_degraded(const std::string& name, std::string& reason,
                           int64_t& since_ms) const {
        CompHealth h; return instance_fault(name, h, reason, since_ms);
    }

    // --- snapshot helpers ----------------------------------------------------

    // The top-level state + its since-ms, atomically. Used by get_health.
    void state_snapshot(SysState& s, int64_t& since_ms) const {
        std::lock_guard<std::mutex> lk(mu_);
        s = state_; since_ms = state_since_ms_;
    }

    // Build one component JSON object (shared by get_health and the event). The
    // caller supplies whatever extra derived fields it wants appended (already
    // formatted as `,"k":v` fragments) via `extra`.
    static void append_component(std::string& out, const char* kind,
                                 const std::string& name, CompHealth health,
                                 const std::string& reason, int64_t since_ms,
                                 const std::string& extra = std::string()) {
        out += "{\"kind\":";        health_json_str(out, kind);
        out += ",\"name\":";        health_json_str(out, name);
        out += ",\"health\":";      health_json_str(out, comp_health_name(health));
        out += ",\"reason_code\":"; health_json_str(out, reason);
        out += ",\"since_ms\":" + std::to_string(since_ms);
        out += extra;
        out += "}";
    }

    // Test/introspection: reset to a clean Boot with no components. NOT used in
    // production (the process starts clean); exists so a test can drive the
    // machine from a known state.
    void reset_for_test() {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = SysState::Boot;
        state_since_ms_ = xi::wall_ms();
        script_present_ = false;
        script_health_ = CompHealth::Ok;
        script_reason_.clear();
        script_name_.clear();
        degraded_.clear();
    }

private:
    HealthRegistry() : state_since_ms_(xi::wall_ms()) {}

    struct DegradedInst { CompHealth health; std::string reason; int64_t since_ms; };
    struct Comp {
        const char* kind; std::string name; CompHealth health;
        std::string reason; int64_t since_ms;
    };

    // While running/degraded, the top-level state tracks whether any runtime
    // health input is unhealthy: the script failed, or any instance carries a
    // runtime fault. (Prepare-time instance failures happen outside running and
    // do not drive this flip — see the design doc.) Called under mu_.
    void recompute_locked_() {
        if (state_ != SysState::Running && state_ != SysState::Degraded) return;
        bool unhealthy = (script_present_ && script_health_ != CompHealth::Ok)
                         || !degraded_.empty();
        SysState want = unhealthy ? SysState::Degraded : SysState::Running;
        if (want != state_) {
            state_ = want;
            state_since_ms_ = xi::wall_ms();
        }
    }

    // Build the health_changed event and fire the notifier OUTSIDE the lock (the
    // send must not hold mu_). Takes the held unique_lock, unlocks it, notifies.
    void emit_(std::unique_lock<std::mutex>& lk, const Comp* comp) {
        std::string ev = "{\"type\":\"event\",\"name\":\"health_changed\",\"data\":{";
        ev += "\"schema\":";     health_json_str(ev, kHealthSchema);
        ev += ",\"state\":";     health_json_str(ev, sys_state_name(state_));
        ev += ",\"since_ms\":" + std::to_string(state_since_ms_);
        if (comp) {
            ev += ",\"component\":{\"kind\":"; health_json_str(ev, comp->kind);
            ev += ",\"name\":";                health_json_str(ev, comp->name);
            ev += ",\"health\":";              health_json_str(ev, comp_health_name(comp->health));
            ev += ",\"reason_code\":";         health_json_str(ev, comp->reason);
            ev += "}";
        }
        ev += ",\"ts_ms\":" + std::to_string(xi::wall_ms());
        ev += "}}";
        Notifier n = notifier_;
        lk.unlock();
        if (n) n(ev);
    }

    mutable std::mutex mu_;
    SysState state_ = SysState::Boot;
    int64_t  state_since_ms_ = 0;

    bool        script_present_ = false;
    CompHealth  script_health_  = CompHealth::Ok;
    std::string script_reason_;
    std::string script_name_;
    int64_t     script_since_ = 0;

    std::map<std::string, DegradedInst> degraded_;   // instance name -> runtime fault
    Notifier notifier_;
};

// Convenience accessor mirroring xi::health() call sites.
inline HealthRegistry& health() { return HealthRegistry::instance(); }

}  // namespace xi

#endif  // XI_HEALTH_HPP
