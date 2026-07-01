#pragma once
//
// xi_record_schema.hpp — a LIGHTWEIGHT, OPT-IN static contract for the Record
// fields a plugin produces / consumes across the plugin boundary.
//
// Motivation (docs/archive/core_fix_plan-2026-07.md §21 "Record cross-plugin
// schema contract", OQ-7): the Record is schemaless — one plugin writes fields
// by string key, another reads them (xi_record.hpp set/get). Today the only
// safety net is the RUNTIME guard `xi::require(in, {"key", ...})` (xi_record.hpp
// :790), which fires per-frame at process() time. This header adds a STATIC
// analogue: a plugin OPTIONALLY declares the field keys+types it produces and
// consumes, and a pipeline is validated at wire/load time so a
// missing-or-wrong-type cross-plugin field is caught ONCE, up front, instead of
// as a per-frame runtime surprise.
//
// MINIMALISM (core_fix_plan.md §27.5 "don't gold-plate"): this is NOT a schema
// system. It is a flat key→coarse-type list + one linear producer→consumer
// check. No nesting, no cardinality, no versioning, no required/optional beyond
// "declared or not".
//
// ABI-FREEZE COMPLIANCE (docs/internals/adr-001-host-api-freeze.md): the
// declaration rides an OPTIONAL PLUGIN EXPORT (xi_plugin_record_schema, resolved
// via GetProcAddress exactly like the ABI-v7 prepare/commit exports). It does
// NOT touch `xi_host_api` — the frozen v11 struct is unchanged — so the freeze
// guard (test_abi_freeze) and golden-plugin test are unaffected. A plugin that
// does NOT export it keeps its current behaviour (declared == false → it imposes
// no constraints and satisfies any consumer).
//

#include "xi_record.hpp"   // brings in vendored yyjson.h + xi::Record

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace xi {

// Coarse Record field types — deliberately few. Mirrors the value shapes the
// Record actually distinguishes (xi_record.hpp: sint / real / bool / str /
// object / array) plus Image (keyed pool handle) and Any (wildcard).
enum class FieldType {
    Any,      // "any"    — wildcard; matches anything (opt-out of the type check)
    Int,      // "int"    — JSON integer (Record::set(int) / as_int)
    Double,   // "double" — JSON real    (Record::set(double) / as_double)
    Bool,     // "bool"   — JSON boolean (Record::set(bool) / as_bool)
    String,   // "string" — JSON string  (Record::set(string) / as_string)
    Image,    // "image"  — a named image handle (Record::image / get_image)
    Record_,  // "record" — a nested object (Record::set(Record))
    Array,    // "array"  — a JSON array   (Record::push)
};

inline FieldType parse_field_type(const std::string& s) {
    if (s == "int"    || s == "integer") return FieldType::Int;
    if (s == "double" || s == "number" || s == "real" || s == "float")
        return FieldType::Double;
    if (s == "bool"   || s == "boolean") return FieldType::Bool;
    if (s == "string" || s == "str")     return FieldType::String;
    if (s == "image")                    return FieldType::Image;
    if (s == "record" || s == "object")  return FieldType::Record_;
    if (s == "array"  || s == "list")    return FieldType::Array;
    return FieldType::Any;   // unknown / "any" / "" → wildcard (never a false fail)
}

inline const char* to_string(FieldType t) {
    switch (t) {
        case FieldType::Int:     return "int";
        case FieldType::Double:  return "double";
        case FieldType::Bool:    return "bool";
        case FieldType::String:  return "string";
        case FieldType::Image:   return "image";
        case FieldType::Record_: return "record";
        case FieldType::Array:   return "array";
        case FieldType::Any:     default: return "any";
    }
}

// Producer type `p` satisfies consumer expectation `c`?
//   - Any on either side → compatible (explicit opt-out).
//   - Int/Double both numeric → compatible (JSON number widening; matches the
//     Record's as_int/as_double coercion, xi_record.hpp:400,420).
//   - otherwise exact match. (So produced=string vs consumed=int is a MISMATCH.)
inline bool types_compatible(FieldType produced, FieldType consumed) {
    if (produced == FieldType::Any || consumed == FieldType::Any) return true;
    const bool p_num = (produced == FieldType::Int || produced == FieldType::Double);
    const bool c_num = (consumed == FieldType::Int || consumed == FieldType::Double);
    if (p_num && c_num) return true;
    return produced == consumed;
}

struct FieldDecl {
    std::string key;
    FieldType   type = FieldType::Any;
};

// One plugin's declared cross-boundary field contract.
struct RecordSchema {
    std::vector<FieldDecl> produces;   // fields this plugin WRITES into its output
    std::vector<FieldDecl> consumes;   // fields this plugin READS from its input
    bool declared = false;             // false ⇒ plugin exported no schema (opt-in)
};

// Parse the schema JSON emitted by xi_plugin_record_schema. Shape (both arrays
// optional; a bare "{}" is a valid empty declaration):
//   {"produces":[{"key":"score","type":"double"}],
//    "consumes":[{"key":"gray","type":"image"},{"key":"thresh","type":"int"}]}
// A field may also be given as a bare string ("score" ≡ {"key":"score",
// "type":"any"}). Returns declared=true on any successful parse (even empty).
inline RecordSchema parse_record_schema_json(const char* json, size_t len) {
    RecordSchema out;
    if (!json || len == 0) return out;   // declared stays false
    yyjson_doc* doc = yyjson_read(json, len, 0);
    if (!doc) return out;                // malformed → treat as undeclared
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) { yyjson_doc_free(doc); return out; }

    auto load_list = [](yyjson_val* arr, std::vector<FieldDecl>& dst) {
        if (!arr || !yyjson_is_arr(arr)) return;
        size_t idx, max;
        yyjson_val* item;
        yyjson_arr_foreach(arr, idx, max, item) {
            FieldDecl fd;
            if (yyjson_is_str(item)) {
                fd.key  = yyjson_get_str(item);
                fd.type = FieldType::Any;
            } else if (yyjson_is_obj(item)) {
                yyjson_val* k = yyjson_obj_get(item, "key");
                if (!k) k = yyjson_obj_get(item, "name");
                yyjson_val* t = yyjson_obj_get(item, "type");
                if (!t) t = yyjson_obj_get(item, "kind");
                if (k && yyjson_is_str(k)) fd.key = yyjson_get_str(k);
                fd.type = (t && yyjson_is_str(t))
                            ? parse_field_type(yyjson_get_str(t)) : FieldType::Any;
            }
            if (!fd.key.empty()) dst.push_back(std::move(fd));
        }
    };
    load_list(yyjson_obj_get(root, "produces"), out.produces);
    load_list(yyjson_obj_get(root, "consumes"), out.consumes);
    // Accept the plugin.json manifest vocabulary too ("outputs"/"inputs"), so
    // the same parser can back a manifest-declared contract later without churn.
    if (out.produces.empty()) load_list(yyjson_obj_get(root, "outputs"), out.produces);
    if (out.consumes.empty()) load_list(yyjson_obj_get(root, "inputs"),  out.consumes);

    out.declared = true;
    yyjson_doc_free(doc);
    return out;
}

// A stage in a wired pipeline: an instance name + its declared schema, in the
// order records flow (upstream first).
struct SchemaStage {
    std::string  name;
    RecordSchema schema;
};

// Validate a wired producer→consumer pipeline. Returns std::nullopt when the
// static field contract holds; otherwise a human-readable message naming the
// FIRST offending stage/field and the reason (missing upstream producer, or
// incompatible type). Fail-loud is the CALLER's choice (throw / XI_FAIL / warn).
//
// OPT-IN semantics (so undeclared plugins keep current behaviour):
//   - A stage with declared==false is an UNKNOWN: it may produce anything, so it
//     is treated as a wildcard producer that can satisfy any later consumer, and
//     it imposes no consume constraints. Its presence upstream suppresses
//     "missing field" errors (we cannot prove absence).
//   - A DECLARED consumer's field is checked against the DECLARED producers seen
//     upstream; a type clash there is always a genuine mismatch and is reported.
inline std::optional<std::string>
validate_record_pipeline(const std::vector<SchemaStage>& stages) {
    struct Avail { FieldType type; std::string producer; };
    std::vector<std::pair<std::string, Avail>> available;   // key → (type, producer)
    bool undeclared_upstream = false;

    auto find = [&](const std::string& key) -> Avail* {
        for (auto& kv : available) if (kv.first == key) return &kv.second;
        return nullptr;
    };

    for (const auto& st : stages) {
        if (!st.schema.declared) {
            // Unknown producer/consumer — cannot constrain, cannot disprove.
            undeclared_upstream = true;
            continue;
        }
        // 1) Check what this stage consumes against upstream producers.
        for (const auto& c : st.schema.consumes) {
            Avail* a = find(c.key);
            if (a) {
                if (!types_compatible(a->type, c.type)) {
                    return "record schema mismatch: field '" + c.key +
                           "' consumed by '" + st.name + "' as " +
                           to_string(c.type) + ", but produced upstream by '" +
                           a->producer + "' as " + to_string(a->type);
                }
            } else if (!undeclared_upstream) {
                return "record schema gap: field '" + c.key +
                       "' consumed by '" + st.name +
                       "' is not produced by any upstream stage";
            }
            // else: an undeclared upstream stage might produce it — allow.
        }
        // 2) Register what this stage produces for downstream consumers.
        for (const auto& p : st.schema.produces) {
            if (Avail* a = find(p.key)) { a->type = p.type; a->producer = st.name; }
            else available.push_back({p.key, Avail{p.type, st.name}});
        }
    }
    return std::nullopt;
}

} // namespace xi
