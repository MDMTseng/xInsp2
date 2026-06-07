#pragma once
//
// xi_pm_parse.hpp — JSON parsing helpers for the plugin/project layer:
// top-level key extraction (extract_string / detail_find_key), a tolerant
// boolean-flag probe (json_flag_true), plugin.json manifest parsing
// (parse_manifest -> PluginInfo), and instance-config validation against a
// plugin's manifest.params (validate_config_against_manifest -> OpenWarning).
//
// Extracted from xi_plugin_manager.hpp: these are pure, stateless functions
// (cJSON + std lib only, no PluginManager state, no mu_), so they belong in a
// leaf header the manager just calls into. Behaviour is unchanged from the
// former static members.
//
#include "cJSON.h"
#include "xi_cabi_adapter.hpp"   // PluginInfo (parse_manifest result)
#include "xi_project_model.hpp"  // OpenWarning (validation diagnostics)

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

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
// `evil` instead of the actual plugin field. cJSON-based parsing
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
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return std::nullopt;
    std::optional<std::string> out;
    cJSON* k = cJSON_GetObjectItem(root, key.c_str());
    if (k) {
        if (cJSON_IsString(k) && k->valuestring) {
            out = std::string(k->valuestring);
        } else if (cJSON_IsNumber(k)) {
            // Prefer integer formatting if the value is integral
            // — many call sites stoi() the result.
            double v = k->valuedouble;
            if (v == (double)(long long)v) {
                out = std::to_string((long long)v);
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.17g", v);
                out = std::string(buf);
            }
        } else if (cJSON_IsBool(k)) {
            out = std::string(cJSON_IsTrue(k) ? "true" : "false");
        }
    }
    cJSON_Delete(root);
    return out;
}

// Extract the raw JSON text of a top-level value (object, array,
// string, scalar) at `key`. Used for opaque blocks like `manifest`
// and `config` that the caller wants to forward verbatim.
inline bool detail_find_key(const std::string& json, const std::string& key,
                            std::string& out) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return false;
    bool found = false;
    cJSON* k = cJSON_GetObjectItem(root, key.c_str());
    if (k) {
        char* printed = cJSON_PrintUnformatted(k);
        if (printed) {
            out.assign(printed);
            std::free(printed);
            found = true;
        }
    }
    cJSON_Delete(root);
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
    pi.folder_path = folder;
    if (pi.has_ui) {
        pi.ui_path = (std::filesystem::path(folder) / "ui").string();
    }
    // Optional manifest block — preserved verbatim. Empty if absent.
    std::string m;
    if (detail_find_key(content, "manifest", m)) pi.manifest_json = std::move(m);
    return pi;
}

// FL r6 P2-3: validate an instance's `config` JSON against the
// plugin's `manifest.params` declarations. Emits one OpenWarning per
// unknown key / type-mismatch / out-of-range / not-in-enum value.
//
// Validation is best-effort and warnings-only: a bad value still
// gets passed to `Plugin::set_def`, which already silently falls
// back to its compiled-in default for unknown / unparseable fields.
// The warning is the user-visible signal that a typo or stale
// value made it through.
//
// Back-compat: if `manifest_json` is empty, doesn't parse, or
// doesn't contain a `params` array, validation is skipped without
// warnings. Plugins predating manifests stay silent.
//
// Pure C++ / cJSON; no platform calls. Safe to share across
// open_project() invocations.
inline void validate_config_against_manifest(
    const std::string& instance,
    const std::string& plugin,
    const std::string& config_json,
    const std::string& manifest_json,
    std::vector<OpenWarning>& out_warnings)
{
    if (manifest_json.empty()) return;
    cJSON* mroot = cJSON_Parse(manifest_json.c_str());
    if (!mroot) return;
    cJSON* params = cJSON_GetObjectItem(mroot, "params");
    if (!params || !cJSON_IsArray(params)) {
        cJSON_Delete(mroot);
        return;
    }
    cJSON* croot = cJSON_Parse(config_json.c_str());
    if (!croot || !cJSON_IsObject(croot)) {
        if (croot) cJSON_Delete(croot);
        cJSON_Delete(mroot);
        return;
    }

    // Build a quick name -> param-decl index. The manifest is small
    // (a few params) so a linear scan would also be fine.
    std::unordered_map<std::string, cJSON*> by_name;
    cJSON* it = nullptr;
    cJSON_ArrayForEach(it, params) {
        if (!cJSON_IsObject(it)) continue;
        cJSON* nm = cJSON_GetObjectItem(it, "name");
        if (nm && cJSON_IsString(nm) && nm->valuestring) {
            by_name[nm->valuestring] = it;
        }
    }

    auto type_of_default = [](cJSON* decl) -> const char* {
        // Prefer explicit "type" if declared; else infer from the
        // "default" value's JSON type. Returns one of:
        // "int", "float", "bool", "string", "" (unknown).
        if (cJSON* t = cJSON_GetObjectItem(decl, "type");
            t && cJSON_IsString(t) && t->valuestring) {
            return t->valuestring;
        }
        cJSON* d = cJSON_GetObjectItem(decl, "default");
        if (!d) return "";
        if (cJSON_IsBool(d))   return "bool";
        if (cJSON_IsString(d)) return "string";
        if (cJSON_IsNumber(d)) {
            // Best-effort split of int vs float based on the literal.
            double v = d->valuedouble;
            if (v == (double)(long long)v) return "int";
            return "float";
        }
        return "";
    };

    auto value_matches_type = [](cJSON* v, const std::string& t) -> bool {
        if (t == "int" || t == "float" || t == "number") {
            return cJSON_IsNumber(v) != 0;
        }
        if (t == "bool" || t == "boolean") {
            return cJSON_IsBool(v) != 0;
        }
        if (t == "string") {
            return cJSON_IsString(v) != 0;
        }
        // Unknown type tag — don't false-positive.
        return true;
    };

    cJSON* cv = nullptr;
    cJSON_ArrayForEach(cv, croot) {
        if (!cv->string) continue;
        const std::string key = cv->string;
        auto pit = by_name.find(key);
        if (pit == by_name.end()) {
            out_warnings.push_back({
                instance, plugin,
                "unknown_config_key: '" + key +
                "' is not declared in plugin manifest.params"
            });
            continue;
        }
        cJSON* decl = pit->second;
        std::string declared_type = type_of_default(decl);
        if (!declared_type.empty() &&
            !value_matches_type(cv, declared_type)) {
            out_warnings.push_back({
                instance, plugin,
                "type_mismatch: config['" + key +
                "'] does not match declared type '" + declared_type + "'"
            });
            // Don't bother with min/max/enum if the type is wrong.
            continue;
        }

        // Numeric range check (min / max).
        if (cJSON_IsNumber(cv)) {
            double v = cv->valuedouble;
            cJSON* mn = cJSON_GetObjectItem(decl, "min");
            cJSON* mx = cJSON_GetObjectItem(decl, "max");
            if (mn && cJSON_IsNumber(mn) && v < mn->valuedouble) {
                out_warnings.push_back({
                    instance, plugin,
                    "out_of_range: config['" + key + "'] = " +
                    std::to_string(v) + " is below declared min " +
                    std::to_string(mn->valuedouble)
                });
            }
            if (mx && cJSON_IsNumber(mx) && v > mx->valuedouble) {
                out_warnings.push_back({
                    instance, plugin,
                    "out_of_range: config['" + key + "'] = " +
                    std::to_string(v) + " is above declared max " +
                    std::to_string(mx->valuedouble)
                });
            }
        }

        // Enum check for strings — declared as a JSON array under
        // "enum". Membership is exact-string.
        if (cJSON_IsString(cv) && cv->valuestring) {
            cJSON* en = cJSON_GetObjectItem(decl, "enum");
            if (en && cJSON_IsArray(en)) {
                bool found = false;
                std::string allowed;
                cJSON* eit = nullptr;
                cJSON_ArrayForEach(eit, en) {
                    if (cJSON_IsString(eit) && eit->valuestring) {
                        if (!allowed.empty()) allowed += ", ";
                        allowed += "'";
                        allowed += eit->valuestring;
                        allowed += "'";
                        if (std::string(eit->valuestring) == cv->valuestring) {
                            found = true;
                        }
                    }
                }
                if (!found) {
                    out_warnings.push_back({
                        instance, plugin,
                        "not_in_enum: config['" + key + "'] = '" +
                        cv->valuestring + "' is not in declared enum {" +
                        allowed + "}"
                    });
                }
            }
        }
        // TODO(p2-3-extend): structured object/array params — current
        // schema only declares scalar params. When manifest.params
        // grows nested-object support, extend the recursion here.
    }

    cJSON_Delete(croot);
    cJSON_Delete(mroot);
}

} // namespace xi
