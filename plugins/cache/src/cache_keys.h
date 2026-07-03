#pragma once
//
// cache_keys.h — the single source of truth for the cache (BufferReplay) plugin's
// command / config / output key names (guard 1 of the plugin data contract).
//
// cache is a generic record intermediary: process() captures WHATEVER record
// arrives into a bounded ring and replays it — it is producer-agnostic by design,
// so the buffered DATA is an OPEN schema (docs/new_gen/02, like record_save /
// expose). There is deliberately no typed view over the buffered payload.
//
// What IS a fixed, declarable vocabulary is the plugin's rich COMMAND surface
// (replay verbs + their params), its CONFIG (ring capacity + read-only status),
// and the small process() acknowledgement it returns. Those key names — parsed
// with xi::Json + string literals before — are named once here; the plugin's own
// readers and the typed view (cache_io.h) compile from these constants.
//

namespace xi::cache {

// Contract schema version. Bump on an incompatible CONFIG change.
inline constexpr int kSchemaVersion = 1;

namespace keys {

// --- Config (get_def / set_def) ---
inline constexpr const char* kCapacity  = "capacity";   // int (settable)
inline constexpr const char* kCount     = "count";      // int  (get_def only, read-only) — TOTAL ring occupancy
inline constexpr const char* kPacks     = "packs";      // int  (get_def only, read-only) — of which are retained packs
inline constexpr const char* kReplaying = "replaying";  // bool (get_def only, read-only)

// --- Commands (exchange) ---
inline constexpr const char* kCommand     = "command";       // string (command selector)
inline constexpr const char* kReplayLast  = "replay_last";   // re-emit last n (default 1)
inline constexpr const char* kReplayAll   = "replay_all";    // re-emit the whole ring
inline constexpr const char* kReplay      = "replay";        // re-emit one at "index" (required)
inline constexpr const char* kReplayTimed = "replay_timed";  // paced replay ("speed")
inline constexpr const char* kStopReplay  = "stop_replay";   // cancel a timed replay
inline constexpr const char* kClear       = "clear";         // empty the ring
inline constexpr const char* kSetCapacity = "set_capacity";  // resize the ring ("value" required)

// --- Command params ---
inline constexpr const char* kN     = "n";      // int   (replay_last count / replay_timed limit)
inline constexpr const char* kIndex = "index";  // int   (replay target — required for "replay")
inline constexpr const char* kSpeed = "speed";  // double (replay_timed pacing scale)
inline constexpr const char* kValue = "value";  // int   (set_capacity payload — required)

// --- process() acknowledgement ---
inline constexpr const char* kBuffered = "buffered";  // int (ring size after capture)

}  // namespace keys
}  // namespace xi::cache
