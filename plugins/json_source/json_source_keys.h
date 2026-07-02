#pragma once
//
// json_source_keys.h — the single source of truth for json_source's COMMAND and
// CONFIG key names (guard 1 of the plugin data contract).
//
// json_source is a generic injector: the JSON it EMITS is user-defined, so its
// output data plane is an OPEN schema by design (docs/new_gen/02, "intrinsically
// generic ... declare open schemas"). There is deliberately no typed Output
// extractor — the produced record is whatever the user authored, plus any
// runtime patches.
//
// What IS a fixed, declarable vocabulary is the plugin's CONTROL surface: the
// exchange COMMANDS, the CONFIG wrapper key, and the PATCH shape its process()
// accepts. Those key names — hand-parsed with raw yyjson before — are named once
// here so the typed view (json_source_io.h) and the plugin's own readers compile
// from the same constants and can't drift.
//

namespace xi::json_source {

// Contract schema version. Bump on an incompatible CONFIG change.
inline constexpr int kSchemaVersion = 1;

namespace keys {

// --- Config (get_def / set_def) ---
// The config wraps the user's JSON under "data" so the UI knows the value is the
// user document, not part of the def itself: { "data": <user json> }.
inline constexpr const char* kData = "data";

// --- Commands (exchange) ---
inline constexpr const char* kCommand   = "command";    // string (command selector)
inline constexpr const char* kSetData   = "set_data";   // replace the stored JSON (payload in "value")
inline constexpr const char* kReset     = "reset";      // clear the stored JSON to {}
inline constexpr const char* kGetStatus = "get_status"; // read-back (falls through to get_def)

// --- Process-input patches (open value) ---
// process() may carry runtime patches that mutate the stored JSON for THIS emit
// only. Two accepted shapes:
//   single: { "key": ".a.b[2]", "value": <anything> }
//   batch:  { "patches": [ { "key": ".x", "value": 1 }, ... ] }
inline constexpr const char* kPatches = "patches";  // array of {key,value}
inline constexpr const char* kKey     = "key";      // patch target path
inline constexpr const char* kValue   = "value";    // patch value / set_data payload (open JSON)

}  // namespace keys
}  // namespace xi::json_source
