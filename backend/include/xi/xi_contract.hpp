#pragma once
//
// xi_contract.hpp — the stable runtime a plugin's data-contract headers call into.
//
// The plugin data contract (docs/new_gen/02-plugin-data-contract.md) keeps the
// schemaless Record as the ONLY thing that crosses the ABI, but gives every
// plugin a typed, discoverable view of its inputs and outputs. Stage 1 ships
// that view as two hand-written headers per plugin:
//
//   <plugin>_keys.h — every input/output/config key name, defined exactly once
//                     (guard 1: the builder, the extractor, AND the plugin's own
//                     reader all compile from it, so a rename can't drift).
//   <plugin>_io.h   — an Input builder (typed setters that compose the input
//                     Record) + an Output extractor (typed getters over the
//                     output Record). Both compile down to plain Record set/get;
//                     nothing new crosses the ABI.
//
// This header is the shared vocabulary those generated/hand-written headers and
// the plugins themselves use for the two behaviours the contract adds on top of
// a raw Record:
//
//   * fail-loud required inputs (guard 2) — a missing/mis-typed required key
//     produces a STRUCTURED failure Record (reason code + key + expected type),
//     riding on the framework's existing NA channel (Record::na) so the script
//     maps it to a verdict. Not a log line, not a silent default.
//   * schema-version skew (guard 3) — the Input builder stamps its compiled-in
//     schema version into the Record ("_schema"); the plugin checks it on
//     process() and, on a header/plugin mismatch, reports a precise error that
//     NAMES BOTH VERSIONS instead of a puzzling "missing key".
//
// Keeping this vocabulary in ONE place (rather than re-deriving the failure
// shape in each plugin) is what makes guard 4 hold: the per-plugin headers stay
// mechanical, so a stage-2 codegen step can regenerate them against this same
// runtime with zero call-site changes.
//
// A structured contract-failure Record looks like:
//   { "$na": "<human reason>",
//     "$fault": { "code": "missing_input"|"wrong_type"|"schema_mismatch",
//                 "key": "<offending key>",            // input faults
//                 "expected_type": "int"|"image"|...,  // input faults
//                 "header_schema": N, "plugin_schema": M } }  // schema faults
//

#include "xi_record.hpp"

#include <initializer_list>
#include <optional>
#include <string>

#include "yyjson.h"

namespace xi::contract {

// --- Reserved keys on a contract-failure Record -----------------------------
// One nested "$fault" object holds the machine-readable detail; the top-level
// "$na" (Record::kNaKey) carries the human reason and makes the failure flow
// through the pipeline as not-available (so a downstream stage / the script
// short-circuits, exactly like any other NA).
inline constexpr const char* kFaultKey = "$fault";

// Fields inside the "$fault" object.
inline constexpr const char* kCode         = "code";
inline constexpr const char* kKey          = "key";
inline constexpr const char* kExpectedType = "expected_type";
inline constexpr const char* kHeaderSchema = "header_schema";
inline constexpr const char* kPluginSchema = "plugin_schema";

// --- Reason codes -----------------------------------------------------------
inline constexpr const char* kMissingInput   = "missing_input";
inline constexpr const char* kWrongType      = "wrong_type";
inline constexpr const char* kSchemaMismatch = "schema_mismatch";

// --- Schema stamp -----------------------------------------------------------
// The Input builder writes its compiled-in schema version here; the plugin
// reads it back in check_schema(). A leading-'_' key (not '$') because it is a
// contract-layer field an author could reasonably inspect, not a framework
// reserved key. warn_unknown_keys() skips '_'/'$'-prefixed keys so this stamp
// is never reported as an unknown input.
inline constexpr const char* kSchemaKey = "_schema";

// --- Declared input/output types --------------------------------------------
enum class Type { Int, Double, Bool, String, Image };

inline const char* type_name(Type t) {
    switch (t) {
        case Type::Int:    return "int";
        case Type::Double: return "double";
        case Type::Bool:   return "bool";
        case Type::String: return "string";
        case Type::Image:  return "image";
    }
    return "unknown";
}

// --- Structured-failure Record builders -------------------------------------

// A required input key is absent.
inline Record missing_input(const char* key, Type expected) {
    Record r = Record::na(std::string("missing required input '") + key +
                          "' (expected " + type_name(expected) + ")");
    r.set(kFaultKey, Record()
        .set(kCode, kMissingInput)
        .set(kKey, key)
        .set(kExpectedType, type_name(expected)));
    return r;
}

// A required input key is present but holds the wrong JSON type.
inline Record wrong_type(const char* key, Type expected) {
    Record r = Record::na(std::string("input '") + key + "' has the wrong type (expected " +
                          type_name(expected) + ")");
    r.set(kFaultKey, Record()
        .set(kCode, kWrongType)
        .set(kKey, key)
        .set(kExpectedType, type_name(expected)));
    return r;
}

// The Record was built by a header schema version this plugin can't serve.
inline Record schema_mismatch(int header_schema, int plugin_schema) {
    Record r = Record::na("plugin data-contract schema mismatch: input built for schema v" +
                          std::to_string(header_schema) + ", this plugin serves schema v" +
                          std::to_string(plugin_schema));
    r.set(kFaultKey, Record()
        .set(kCode, kSchemaMismatch)
        .set(kHeaderSchema, header_schema)
        .set(kPluginSchema, plugin_schema));
    return r;
}

// The exchange()/set_def() analogue of the failure Record: a JSON string, since
// those C-ABI verbs hand a string back rather than a Record. Same reason-code
// vocabulary so a UI/driver sees one error shape across both channels.
inline std::string fault_json(const char* code, const char* key, const char* expected_type) {
    std::string out = "{\"error\":\"";
    out += code;
    out += "\"";
    if (key)           { out += ",\"key\":\"";           out += key;           out += "\""; }
    if (expected_type) { out += ",\"expected_type\":\""; out += expected_type; out += "\""; }
    out += "}";
    return out;
}

// --- Schema check -----------------------------------------------------------

// Compare the input's stamped schema (written by the Input builder) against the
// version this plugin was compiled to serve. Stage-1 policy:
//   * key absent   → tolerate (a hand-built Record that didn't use the builder);
//   * equal        → proceed;
//   * different    → precise schema_mismatch naming both versions.
// Returns nullopt when it is safe to proceed.
inline std::optional<Record> check_schema(const Record& in, int plugin_schema) {
    if (!in.has(kSchemaKey)) return std::nullopt;
    int hv = in.get_int(kSchemaKey, -1);
    if (hv == plugin_schema) return std::nullopt;
    return schema_mismatch(hv, plugin_schema);
}

// --- Required-input checks --------------------------------------------------
// Each returns nullopt when the key is present with the expected type, else the
// structured failure Record to return straight out of process().

inline std::optional<Record> require(const Record& in, const char* key, Type expected) {
    if (!in.has(key)) return missing_input(key, expected);
    Record::Value v = in[key];
    bool ok = false;
    switch (expected) {
        case Type::Int:    ok = v.is_number(); break;
        case Type::Double: ok = v.is_number(); break;
        case Type::Bool:   ok = v.is_bool();   break;
        case Type::String: ok = v.is_string(); break;
        case Type::Image:  ok = false;         break;  // images use require_image
    }
    if (!ok) return wrong_type(key, expected);
    return std::nullopt;
}

// Required IMAGE input (images live in the Record's image bag, not its JSON).
// channels == 0 accepts any channel count; otherwise the count must match.
inline std::optional<Record> require_image(const Record& in, const char* key, int channels = 0) {
    const Image& img = in.get_image(key);
    if (img.empty()) return missing_input(key, Type::Image);
    if (channels != 0 && img.channels != channels) return wrong_type(key, Type::Image);
    return std::nullopt;
}

// --- Unknown-key tolerance (guard 2 tail) -----------------------------------
// Schemaless tolerance is retained: extra keys are IGNORED. But an author who
// mis-typed a key name gets one warning rather than silence. Iterate the input's
// JSON keys once; any key not in `known` (and not a framework "$"/contract key)
// is reported through `log` exactly once per instance (the caller owns the
// `warned` latch). Cheap: skipped entirely once `warned` is set.
template <class LogFn>
inline void warn_unknown_keys(const Record& in,
                              std::initializer_list<const char*> known,
                              bool& warned, LogFn&& log) {
    if (warned) return;
    yyjson_mut_val* root = in.json();
    if (!root || !yyjson_mut_is_obj(root)) return;
    std::string unknown;
    yyjson_mut_obj_iter it;
    yyjson_mut_obj_iter_init(root, &it);
    yyjson_mut_val* k;
    while ((k = yyjson_mut_obj_iter_next(&it))) {
        const char* name = yyjson_mut_get_str(k);
        if (!name) continue;
        if (name[0] == '$' || name[0] == '_') continue;  // framework / contract keys
        bool found = false;
        for (const char* kn : known) {
            if (std::strcmp(name, kn) == 0) { found = true; break; }
        }
        if (!found) { if (!unknown.empty()) unknown += ", "; unknown += name; }
    }
    if (!unknown.empty()) {
        log(std::string("ignoring unknown input key(s): ") + unknown);
        warned = true;
    }
}

}  // namespace xi::contract
