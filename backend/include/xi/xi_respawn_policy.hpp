#pragma once
//
// xi_respawn_policy.hpp — the FE supervisor's rate-limited respawn decision.
//
// Extracted from fe_main.cpp so the unit tests exercise the REAL function rather
// than a hand-copied model of it (test_qa_fault / test_qa_race). Pure + portable
// (only <algorithm>/<cstdint>/<vector>); no Win32.
//
// Mirrors the VS Code extension's policy: at most `max` respawns inside a
// sliding `window_ms`; beyond that the FE stays safe and waits for a human.
//
#include <algorithm>
#include <cstdint>
#include <vector>

namespace xi {

// Decide what to do when the backend has just died.
//
//   `respawns`  — timestamps (ms) of recent respawns; pruned in place to the
//                 window, and the current death appended when a respawn is allowed.
//   `now`       — current time (ms, same clock as the stored timestamps).
//   `window_ms` — sliding window width.
//   `max`       — max respawns permitted within the window.
//
// Returns true if this death TRIPS the cap (stay safe, do NOT respawn); false if
// a respawn is allowed (and `now` has been recorded).
inline bool respawn_should_trip(std::vector<int64_t>& respawns, int64_t now,
                                int window_ms, int max) {
    respawns.erase(std::remove_if(respawns.begin(), respawns.end(),
                   [&](int64_t ts) { return now - ts > window_ms; }),
                   respawns.end());
    if ((int)respawns.size() >= max) return true;   // cap reached
    respawns.push_back(now);
    return false;
}

} // namespace xi
