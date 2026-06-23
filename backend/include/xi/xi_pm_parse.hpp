#pragma once
//
// xi_pm_parse.hpp — JSON parsing helpers for the plugin/project layer:
// top-level key extraction (extract_string / detail_find_key), a tolerant
// boolean-flag probe (json_flag_true), and plugin.json manifest parsing
// (parse_manifest -> PluginInfo).
//
// Extracted from xi_plugin_manager.hpp: these are pure, stateless functions
// (yyjson + std lib only, no PluginManager state, no mu_), so they belong in a
// leaf header the manager just calls into. Behaviour is unchanged from the
// former static members.
//
// (Instance-config validation against manifest.params — an opt-in diagnostic,
// not part of loading — lives in its own leaf xi_config_validate.hpp.)
//
#include "yyjson.h"
#include "xi_cabi_adapter.hpp"   // PluginInfo (parse_manifest result)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace xi {

inline bool json_flag_true(const std::string& s, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    return s.find(k + ":true") != std::string::npos ||
           s.find(k + ": true") != std::string::npos;
}

// D-P1-2: top-level top-key extraction. Previously implemented as a
// substring search which matched the key text anywhere — including
// inside another value's string content. A plugin description
// containing `"plugin": "evil"` would cause downstream code to load
// `evil` instead of the actual plugin field. yyjson-based parsing
// closes that hole; only top-level object keys are honoured.
//
// Returns nullopt on parse failure / missing key / wrong type
// (matches the pre-fix contract for "treat as missing"). Numbers
// declared as JSON numbers are accepted via stringification so
// legitimate `"call_timeout_ms": 5000` works without a quoted
// string (closes the audit's secondary observation that the old
// helper expected quotes for numeric fields).
inline std::optional<std::string> extract_string(const std::string& json,
                                                  const std::string& key) {
    yyjson_doc* doc = yyjson_read(json.c_str(), json.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!root) { if (doc) yyjson_doc_free(doc); return std::nullopt; }
    std::optional<std::string> out;
    yyjson_val* k = yyjson_obj_get(root, key.c_str());
    if (k) {
        if (yyjson_is_str(k) && yyjson_get_str(k)) {
            out = std::string(yyjson_get_str(k));
        } else if (yyjson_is_num(k)) {
            // Prefer integer formatting if the value is integral
            // — many call sites stoi() the result.
            double v = yyjson_get_num(k);
            if (v == (double)(long long)v) {
                out = std::to_string((long long)v);
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.17g", v);
                out = std::string(buf);
            }
        } else if (yyjson_is_bool(k)) {
            out = std::string(yyjson_get_bool(k) ? "true" : "false");
        }
    }
    yyjson_doc_free(doc);
    return out;
}

// Extract the raw JSON text of a top-level value (object, array,
// string, scalar) at `key`. Used for opaque blocks like `manifest`
// and `config` that the caller wants to forward verbatim.
inline bool detail_find_key(const std::string& json, const std::string& key,
                            std::string& out) {
    yyjson_doc* doc = yyjson_read(json.c_str(), json.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!root) { if (doc) yyjson_doc_free(doc); return false; }
    bool found = false;
    yyjson_val* k = yyjson_obj_get(root, key.c_str());
    if (k) {
        char* printed = yyjson_val_write(k, 0, NULL);
        if (printed) {
            out.assign(printed);
            std::free(printed);
            found = true;
        }
    }
    yyjson_doc_free(doc);
    return found;
}

inline PluginInfo parse_manifest(const std::string& path, const std::string& folder) {
    PluginInfo pi;
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    auto name = extract_string(content, "name");
    auto desc = extract_string(content, "description");
    auto dll  = extract_string(content, "dll");
    auto fact = extract_string(content, "factory");

    if (name) pi.name = *name;
    if (desc) pi.description = *desc;
    if (dll)  pi.dll_name = *dll;
    else      pi.dll_name = pi.name + ".dll";
    if (fact) pi.factory_symbol = *fact;
    else      pi.factory_symbol = "xi_plugin_create";

    pi.has_ui = (content.find("\"has_ui\":true") != std::string::npos) ||
                (content.find("\"has_ui\": true") != std::string::npos);
    pi.reentrant = json_flag_true(content, "reentrant") ||
                   json_flag_true(content, "thread_safe");  // documented alias
    pi.json_fallback = json_flag_true(content, "json_fallback");
    // Build mode: `"build": "cmake"` (alias `"prebuilt": true`) means the plugin
    // owns its build — the backend loads its prebuilt `build/<name>.dll` and never
    // invokes cl.exe on it. Default (absent / "source") = backend compiles it.
    pi.prebuilt = json_flag_true(content, "prebuilt") ||
                  (extract_string(content, "build").value_or("") == "cmake");
    pi.folder_path = folder;
    if (pi.has_ui) {
        pi.ui_path = (std::filesystem::path(folder) / "ui").string();
    }
    // Optional manifest block — preserved verbatim. Empty if absent.
    std::string m;
    if (detail_find_key(content, "manifest", m)) pi.manifest_json = std::move(m);
    return pi;
}

} // namespace xi
