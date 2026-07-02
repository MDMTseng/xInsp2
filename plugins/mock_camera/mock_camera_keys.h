#pragma once
//
// mock_camera_keys.h — the single source of truth for mock_camera's Record /
// command key names (guard 1 of the plugin data contract).
//
// mock_camera is a SOURCE plugin: it has no process() input Record. Its
// script/UI-facing contract is therefore its CONFIG (get_def/set_def), its
// COMMANDS (exchange), and the OUTPUT frame it emits. Every key name on those
// three surfaces is named exactly once here; the plugin's own readers and the
// typed view (mock_camera_io.h) both compile from these constants.
//
// Bump kSchemaVersion on an incompatible CONFIG change; the Config builder
// stamps it and set_def rejects a mismatched build with a precise error.
//

namespace xi::mock_camera {

// Contract schema version. Bump on an incompatible CONFIG change.
inline constexpr int kSchemaVersion = 1;

namespace keys {

// --- Config (get_def / set_def) ---
inline constexpr const char* kWidth     = "width";      // int
inline constexpr const char* kHeight    = "height";     // int
inline constexpr const char* kFps       = "fps";        // int
inline constexpr const char* kStreaming = "streaming";  // bool (get_def only, read-only)
// polaris2 wave-2 (docs/new_gen/08 Wave 2): emit on the v3 Frame plane
// (xi.frame@1) instead of a Record. DEFAULT OFF — the Record emit path is the
// unchanged default; only a project that opts in rides the frame door.
inline constexpr const char* kFrameMode = "frame_mode"; // bool (default false)

// --- Commands (exchange) ---
inline constexpr const char* kCommand   = "command";    // string (command selector)
inline constexpr const char* kValue     = "value";      // int (set_fps payload)
inline constexpr const char* kStart         = "start";
inline constexpr const char* kStop          = "stop";
inline constexpr const char* kGetStatus     = "get_status";
inline constexpr const char* kSetFps        = "set_fps";
inline constexpr const char* kSetResolution = "set_resolution";

// --- Output frame ---
inline constexpr const char* kFrame     = "frame";      // image (emitted RGB frame)
inline constexpr const char* kSeq       = "seq";        // int (frame counter; frame-mode entry)

}  // namespace keys
}  // namespace xi::mock_camera
