#pragma once
//
// config_swap_probe_keys.h — the single source of truth for config_swap_probe's
// config / command / status key names (guard 1 of the plugin data contract).
//
// config_swap_probe is a reference plugin for the orchestrator config-swap design:
// its process() ignores the input Record and returns an empty one (it has no
// process data contract — nothing to require, nothing to extract). Its real
// script/UI-facing surface is its CONFIG ({value}), its one exchange COMMAND
// (get_status), and the STATUS object that command returns. Every key on those
// surfaces is named once here; the plugin's own readers and the typed view
// (config_swap_probe_io.h) compile from these constants.
//
// Bump kSchemaVersion on an incompatible CONFIG change; the Config builder stamps
// it so a header/plugin skew reports precisely.
//

namespace xi::config_swap_probe {

// Contract schema version. Bump on an incompatible CONFIG change.
inline constexpr int kSchemaVersion = 1;

namespace keys {

// --- Config (get_def / set_def / prepare) ---
inline constexpr const char* kValue = "value";   // int — the loaded resource value

// --- Commands (exchange) ---
inline constexpr const char* kCommand   = "command";     // string (reserved; probe reads get_status)
inline constexpr const char* kGetStatus = "get_status";  // read the double-slot status

// --- Status object (get_status reply) ---
inline constexpr const char* kActive      = "active";       // int  — value in the LIVE slot
inline constexpr const char* kStaged      = "staged";       // bool — is a resource staged?
inline constexpr const char* kStagedValue = "staged_value"; // int  — value in the STAGING slot (-1 if none)
inline constexpr const char* kLastSeen    = "last_seen";    // int  — value the last process() observed
inline constexpr const char* kProc        = "proc";         // int  — process() call count

}  // namespace keys
}  // namespace xi::config_swap_probe
