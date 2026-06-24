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

} // namespace xi
