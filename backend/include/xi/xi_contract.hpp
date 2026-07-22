#pragma once
//
// xi_contract.hpp — the shared data-contract VOCABULARY (reason codes, declared
// types, and the string-channel fault shape) a plugin's typed-io headers reuse.
//
// [ABI v12 — THE CUT] The Record-typed half of this header (the structured
// failure-Record builders missing_input / wrong_type / schema_mismatch, the
// check_schema / require / require_image guards, and warn_unknown_keys) was
// DELETED together with xi::Record. A plugin's data plane is now the xi.pack@1
// door, and the PACK-shaped equivalent of fail-loud + provenance lives in
// xi_pack_contract.hpp (xi::pack_contract) — which reuses the SAME reason-code
// vocabulary defined below so a UI/driver still sees one error shape across the
// Record-era string channel and the pack channel.
//
// What remains here is the ABI-neutral vocabulary that never crossed as a
// Record: the reserved $fault field names, the reason codes, the declared
// input/output Type enum, and fault_json() — the exchange()/set_def() analogue
// that hands a JSON STRING back rather than a Record.
//

#include <string>

namespace xi::contract {

// --- Reserved keys on a contract-failure carrier ----------------------------
// One nested "$fault" object holds the machine-readable detail; the top-level
// "$na" carries the human reason. The pack channel mirrors these in
// xi_pack_contract.hpp with the same reason codes.
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
// The Input builder writes its compiled-in schema version under this key; the
// plugin reads it back to detect header/plugin skew. A leading-'_' key (not '$')
// because it is a contract-layer field an author could reasonably inspect.
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

// The exchange()/set_def() fault carrier: a JSON string, since those C-ABI verbs
// hand a string back. Same reason-code vocabulary as the pack channel so a
// UI/driver sees one error shape across both.
inline std::string fault_json(const char* code, const char* key, const char* expected_type) {
    std::string out = "{\"error\":\"";
    out += code;
    out += "\"";
    if (key)           { out += ",\"key\":\"";           out += key;           out += "\""; }
    if (expected_type) { out += ",\"expected_type\":\""; out += expected_type; out += "\""; }
    out += "}";
    return out;
}

}  // namespace xi::contract
