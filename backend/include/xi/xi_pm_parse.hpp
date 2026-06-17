#pragma once
//
// xi_pm_parse.hpp — JSON parsing helpers for the plugin/project layer:
// top-level key extraction (extract_string / detail_find_key), a tolerant
// boolean-flag probe (json_flag_true), plugin.json manifest parsing
// (parse_manifest -> PluginInfo), and instance-config validation against a
// plugin's manifest.params (validate_config_against_manifest -> OpenWarning).
//
// Extracted from xi_plugin_manager.hpp: these are pure, stateless functions
// (yyjson + std lib only, no PluginManager state, no mu_), so they belong in a
// leaf header the manager just calls into. Behaviour is unchanged from the
// former static members.
//
#include "yyjson.h"
#include "xi_cabi_adapter.hpp"   // PluginInfo (parse_manifest result)
#include "xi_project_model.hpp"  // OpenWarning (validation diagnostics)

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
// Pure C++ / yyjson; no platform calls. Safe to share across
// open_project() invocations.
inline void validate_config_against_manifest(
    const std::string& instance,
    const std::string& plugin,
    const std::string& config_json,
    const std::string& manifest_json,
    std::vector<OpenWarning>& out_warnings)
{
    if (manifest_json.empty()) return;
    yyjson_doc* mdoc = yyjson_read(manifest_json.c_str(), manifest_json.size(), 0);
    yyjson_val* mroot = mdoc ? yyjson_doc_get_root(mdoc) : nullptr;
    if (!mroot) { if (mdoc) yyjson_doc_free(mdoc); return; }
    yyjson_val* params = yyjson_obj_get(mroot, "params");
    if (!params || !yyjson_is_arr(params)) {
        yyjson_doc_free(mdoc);
        return;
    }
    yyjson_doc* cdoc = yyjson_read(config_json.c_str(), config_json.size(), 0);
    yyjson_val* croot = cdoc ? yyjson_doc_get_root(cdoc) : nullptr;
    if (!croot || !yyjson_is_obj(croot)) {
        if (cdoc) yyjson_doc_free(cdoc);
        yyjson_doc_free(mdoc);
        return;
    }

    // Build a quick name -> param-decl index. The manifest is small
    // (a few params) so a linear scan would also be fine.
    std::unordered_map<std::string, yyjson_val*> by_name;
    {
        size_t _i, _n; yyjson_val* it;
        yyjson_arr_foreach(params, _i, _n, it) {
            if (!yyjson_is_obj(it)) continue;
            yyjson_val* nm = yyjson_obj_get(it, "name");
            if (nm && yyjson_is_str(nm) && yyjson_get_str(nm)) {
                by_name[yyjson_get_str(nm)] = it;
            }
        }
    }

    auto type_of_default = [](yyjson_val* decl) -> const char* {
        // Prefer explicit "type" if declared; else infer from the
        // "default" value's JSON type. Returns one of:
        // "int", "float", "bool", "string", "" (unknown).
        if (yyjson_val* t = yyjson_obj_get(decl, "type");
            t && yyjson_is_str(t) && yyjson_get_str(t)) {
            return yyjson_get_str(t);
        }
        yyjson_val* d = yyjson_obj_get(decl, "default");
        if (!d) return "";
        if (yyjson_is_bool(d))   return "bool";
        if (yyjson_is_str(d)) return "string";
        if (yyjson_is_num(d)) {
            // Best-effort split of int vs float based on the literal.
            double v = yyjson_get_num(d);
            if (v == (double)(long long)v) return "int";
            return "float";
        }
        return "";
    };

    auto value_matches_type = [](yyjson_val* v, const std::string& t) -> bool {
        if (t == "int" || t == "float" || t == "number") {
            return yyjson_is_num(v) != 0;
        }
        if (t == "bool" || t == "boolean") {
            return yyjson_is_bool(v) != 0;
        }
        if (t == "string") {
            return yyjson_is_str(v) != 0;
        }
        // Unknown type tag — don't false-positive.
        return true;
    };

    size_t _ci, _cn; yyjson_val *ckey, *cv;
    yyjson_obj_foreach(croot, _ci, _cn, ckey, cv) {
        const char* ckeystr = yyjson_get_str(ckey);
        if (!ckeystr) continue;
        const std::string key = ckeystr;
        auto pit = by_name.find(key);
        if (pit == by_name.end()) {
            out_warnings.push_back({
                instance, plugin,
                "unknown_config_key: '" + key +
                "' is not declared in plugin manifest.params"
            });
            continue;
        }
        yyjson_val* decl = pit->second;
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
        if (yyjson_is_num(cv)) {
            double v = yyjson_get_num(cv);
            yyjson_val* mn = yyjson_obj_get(decl, "min");
            yyjson_val* mx = yyjson_obj_get(decl, "max");
            if (mn && yyjson_is_num(mn) && v < yyjson_get_num(mn)) {
                out_warnings.push_back({
                    instance, plugin,
                    "out_of_range: config['" + key + "'] = " +
                    std::to_string(v) + " is below declared min " +
                    std::to_string(yyjson_get_num(mn))
                });
            }
            if (mx && yyjson_is_num(mx) && v > yyjson_get_num(mx)) {
                out_warnings.push_back({
                    instance, plugin,
                    "out_of_range: config['" + key + "'] = " +
                    std::to_string(v) + " is above declared max " +
                    std::to_string(yyjson_get_num(mx))
                });
            }
        }

        // Enum check for strings — declared as a JSON array under
        // "enum". Membership is exact-string.
        if (yyjson_is_str(cv) && yyjson_get_str(cv)) {
            yyjson_val* en = yyjson_obj_get(decl, "enum");
            if (en && yyjson_is_arr(en)) {
                bool found = false;
                std::string allowed;
                size_t _ei, _en2; yyjson_val* eit;
                yyjson_arr_foreach(en, _ei, _en2, eit) {
                    if (yyjson_is_str(eit) && yyjson_get_str(eit)) {
                        if (!allowed.empty()) allowed += ", ";
                        allowed += "'";
                        allowed += yyjson_get_str(eit);
                        allowed += "'";
                        if (std::string(yyjson_get_str(eit)) == yyjson_get_str(cv)) {
                            found = true;
                        }
                    }
                }
                if (!found) {
                    out_warnings.push_back({
                        instance, plugin,
                        "not_in_enum: config['" + key + "'] = '" +
                        yyjson_get_str(cv) + std::string("' is not in declared enum {") +
                        allowed + "}"
                    });
                }
            }
        }
        // TODO(p2-3-extend): structured object/array params — current
        // schema only declares scalar params. When manifest.params
        // grows nested-object support, extend the recursion here.
    }

    yyjson_doc_free(cdoc);
    yyjson_doc_free(mdoc);
}

} // namespace xi
