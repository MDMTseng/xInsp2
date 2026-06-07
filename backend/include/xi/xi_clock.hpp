#pragma once
//
// xi_clock.hpp — the single home for time. Two clocks, named by INTENT so the
// wall-vs-monotonic choice is made (and visible) at the call site:
//
//   wall_*  — WALL clock (system_clock). For timestamps only: human-facing or
//             cross-record values (status/crash ts_ms, trigger timestamp_us,
//             dequeue/log times). MAY jump on an NTP/DST correction.
//   mono_*  — MONOTONIC clock (steady_clock). For deadlines/intervals only:
//             watchdog, rate limit, correlation window, heartbeat staleness,
//             respawn/boot timeouts, dead-man latch. NEVER jumps.
//
// RULE: never subtract a wall_* from a mono_* (or vice versa) — they have
// unrelated epochs. Picking a deadline? use mono_*. Stamping an event? wall_*.
//
// Header-only, std::chrono only — no platform coupling. (Cross-platform clean;
// no TODO(linux) needed.)
//
#include <chrono>
#include <cstdint>

namespace xi {

inline int64_t wall_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
inline int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
inline int64_t mono_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline int64_t mono_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace xi
