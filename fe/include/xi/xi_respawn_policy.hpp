#pragma once
//
// xi_respawn_policy.hpp — the FE supervisor's respawn-cap decision.
//
// Extracted from fe_main.cpp so the unit tests exercise the REAL logic, not a
// hand-copied model (test_qa_fault / test_qa_race). Pure + portable; no Win32.
//
// Policy: count CONSECUTIVE failures and latch safe once they exceed `max`. The
// counter resets only after the backend has stayed continuously healthy long
// enough to count as recovered. This catches a recurring fault whether the
// deaths are fast OR slow.
//
//   (The earlier sliding-time-window cap had a safety hole — a slow crash-loop
//    whose deaths landed farther apart than window/max never accumulated `max`
//    inside the window, so the FE thrashed forever and never latched safe.)
//
#include <cstdint>
#include <string>
#include <unordered_map>

namespace xi {

struct RespawnTracker {
    int consecutive = 0;   // failures since the last sustained-healthy period

    // Observe that the backend is currently healthy. `healthy_for_ms` is how
    // long it has been continuously healthy this instance; once that reaches
    // `reset_ms` the line has genuinely recovered, so forget prior failures.
    void note_healthy(int64_t healthy_for_ms, int64_t reset_ms) {
        if (healthy_for_ms >= reset_ms) consecutive = 0;
    }

    // Record an unexpected death. Returns true if the respawn cap is now
    // EXCEEDED (latch safe, stop respawning); false if a respawn is allowed.
    // `max` respawns are allowed; the (max+1)-th consecutive death trips.
    bool note_death(int max) { return ++consecutive > max; }

    void reset() { consecutive = 0; }
};

// Part III G2.2 — per-PLUGIN crash budget, keyed by plugin id. The process-wide
// RespawnTracker above graduates the supervisor's lever from "respawn the whole
// backend" to "give up on the whole line"; this graduates it once more, to
// "disable the one offending plugin, keep the line up" (Invariant §20.2).
//
// Policy mirrors RespawnTracker but per key: count deaths attributed to plugin X
// (crash report culprit, cross-checked against the faulting module) and trip once
// they reach `max` within a sliding time WINDOW. A window (not RespawnTracker's
// sustained-healthy reset) is the right model here because attributed faults are
// sparse — a plugin that crashes the backend a few times an hour over a shift is
// exactly the flaky-plugin case to quarantine, even though the backend is
// "healthy" (running other instances) in between. Pure + portable; no Win32, so
// the FE unit test exercises the real logic, not a copy.
struct PluginFaultTracker {
    int64_t window_ms = 600'000;   // 10 min sliding window
    int     max       = 3;         // faults within the window before quarantine

    // Record one attributed fault for `plugin` at time `now_ms`. Returns true when
    // this fault makes the plugin TRIP its budget (>= max within the window) for
    // the first time — the caller then quarantines it. Subsequent faults after a
    // trip return false (already quarantined; don't re-act).
    bool note_fault(const std::string& plugin, int64_t now_ms) {
        if (plugin.empty()) return false;
        auto& s = state_[plugin];
        // Slide the window: drop the count if the last fault is older than window.
        if (now_ms - s.last_ms > window_ms) s.count = 0;
        s.last_ms = now_ms;
        ++s.count;
        if (s.tripped) return false;        // already quarantined — act once
        if (s.count >= max) { s.tripped = true; return true; }
        return false;
    }

    // Clear a plugin's budget (operator un-quarantined it, or its DLL was rebuilt).
    void clear(const std::string& plugin) { state_.erase(plugin); }

    bool tripped(const std::string& plugin) const {
        auto it = state_.find(plugin);
        return it != state_.end() && it->second.tripped;
    }
    int count(const std::string& plugin) const {
        auto it = state_.find(plugin);
        return it == state_.end() ? 0 : it->second.count;
    }

private:
    struct St { int count = 0; int64_t last_ms = 0; bool tripped = false; };
    std::unordered_map<std::string, St> state_;
};

} // namespace xi
