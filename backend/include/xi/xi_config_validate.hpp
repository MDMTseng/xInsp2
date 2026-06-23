#pragma once
//
// xi_config_validate.hpp — validate an instance's `config` JSON against the
// plugin's manifest.params declarations (-> OpenWarning diagnostics).
//
// Extracted from xi_pm_parse.hpp: manifest *parsing* (parse_manifest) is core —
// the loader needs it to discover a plugin. Config *validation* is an opt-in
// diagnostic: it never changes what loads (a bad value still falls back to the
// plugin's compiled-in default via set_def), it only emits a warning so a typo
// or stale value is visible. So it lives in its own leaf the manager calls into.
//
// Pure C++ / yyjson; no platform calls, no PluginManager state. Safe to share
// across open_project() invocations.
//
#include "yyjson.h"
#include "xi_project_model.hpp"  // OpenWarning (validation diagnostics)

#include <string>
#include <unordered_map>
#include <vector>

namespace xi {

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
            // Only SCALARS are tunables. A get_def() legitimately persists
            // structured state (e.g. a `templates` array, a nested object) that
            // isn't a manifest.param — don't warn on those (was DM-8: authors had
            // to declare fake description-only pseudo-params to silence it, which
            // polluted the tunables surface tooling/agents read).
            if (yyjson_is_arr(cv) || yyjson_is_obj(cv)) continue;
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
