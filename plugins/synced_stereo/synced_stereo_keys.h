#pragma once
//
// synced_stereo_keys.h — the single source of truth for synced_stereo's Record /
// command key names (guard 1 of the plugin data contract).
//
// synced_stereo is a GATHERING SOURCE: it has no process() input Record. Its
// script/UI-facing contract is therefore its CONFIG (get_def/set_def), its
// COMMANDS (exchange), and the two-image FRAME it emits (left + right under one
// trigger). Every key name on those three surfaces is named exactly once here;
// the typed view (synced_stereo_io.h) and the plugin's own readers compile from
// these constants, so a rename can't drift between the wrapper and the plugin.
//
// NOTE: synced_stereo.cpp is NOT yet compiled against these constants — its
// worker/emit path is being ported to the blessed spawn_worker on a parallel
// branch, so this task deliberately does not touch it to avoid a conflicting
// diff. The values below therefore mirror the literals synced_stereo.cpp emits
// today; the host-mock test pins that wire contract, so a drift fails the test.
//
// Bump kSchemaVersion on an incompatible CONFIG change; the Config builder
// stamps it (mock_camera-style) so a header/plugin skew reports precisely.
//

namespace xi::synced_stereo {

// Contract schema version. Bump on an incompatible CONFIG change.
inline constexpr int kSchemaVersion = 1;

namespace keys {

// --- Config (get_def / set_def) ---
inline constexpr const char* kFps     = "fps";      // int  (settable)
inline constexpr const char* kRunning = "running";  // bool (get_def only, read-only)
inline constexpr const char* kTicks   = "ticks";    // int  (get_def only, read-only)

// --- Commands (exchange) ---
inline constexpr const char* kCommand = "command";  // string (command selector)
inline constexpr const char* kValue   = "value";    // int (set_fps payload)
inline constexpr const char* kN       = "n";        // int (fire count)
inline constexpr const char* kStart   = "start";
inline constexpr const char* kStop    = "stop";
inline constexpr const char* kFire    = "fire";     // deterministic headless drive
inline constexpr const char* kSetFps  = "set_fps";

// --- Output frame (one record carries BOTH images under one trigger) ---
inline constexpr const char* kLeft    = "left";     // image (vertical stripes)
inline constexpr const char* kRight   = "right";    // image (horizontal stripes)

}  // namespace keys
}  // namespace xi::synced_stereo
