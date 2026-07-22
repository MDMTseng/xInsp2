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
#include "xi_dynlib.hpp"         // platform_module_path (.dll -> .so/.dylib)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace xi {

// D-P1-2 (extended to flags): a tolerant boolean-flag probe that honours ONLY a
// top-level object key. The former implementation was a flat substring scan
// (`s.find("\"reentrant\":true")`) over the WHOLE manifest, so a `"reentrant":true`
// buried in a nested `manifest` example block or a description string produced a
// false positive — silently DISABLING per-instance serialization for a
// non-reentrant plugin (concurrent process() → data race). yyjson, top-level only,
// closes that hole (mirrors extract_string's hardening). Accepts a real JSON bool;
// also tolerates a quoted "true"/"false" string so manifests that stringify the
// flag still work. Missing / wrong-type / parse-fail → false (treat as off).
inline bool json_flag_true(const std::string& s, const char* key) {
    yyjson_doc* doc = yyjson_read(s.c_str(), s.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    bool result = false;
    if (root) {
        yyjson_val* k = yyjson_obj_get(root, key);
        if (k && yyjson_is_bool(k)) {
            result = yyjson_get_bool(k);
        } else if (k && yyjson_is_str(k) && yyjson_get_str(k)) {
            result = (std::string(yyjson_get_str(k)) == "true");
        }
    }
    if (doc) yyjson_doc_free(doc);
    return result;
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

// Parse an LV2-style capability-handshake array (`requires` / `optional`) from a
// plugin.json: a top-level array of `{"iface":"xi.imaging","min":1}` objects.
// `min` is optional and defaults to 1 (the lowest published interface version);
// an entry with no/blank `iface` string is skipped. Missing key / wrong type /
// parse-fail → empty (treat as "no declared capabilities", the common case).
// Top-level only (same hardening as extract_string/json_flag_true) so an example
// buried in a nested `manifest` block can't fabricate a requirement.
inline std::vector<IfaceReq> parse_iface_reqs(const std::string& json,
                                              const char* key) {
    std::vector<IfaceReq> out;
    yyjson_doc* doc = yyjson_read(json.c_str(), json.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (root) {
        yyjson_val* arr = yyjson_obj_get(root, key);
        if (arr && yyjson_is_arr(arr)) {
            size_t idx, max; yyjson_val* el;
            yyjson_arr_foreach(arr, idx, max, el) {
                if (!yyjson_is_obj(el)) continue;
                yyjson_val* ifv = yyjson_obj_get(el, "iface");
                if (!ifv || !yyjson_is_str(ifv) || !yyjson_get_str(ifv)) continue;
                IfaceReq r;
                r.iface = yyjson_get_str(ifv);
                if (r.iface.empty()) continue;
                yyjson_val* mn = yyjson_obj_get(el, "min");
                if (mn && yyjson_is_int(mn) && yyjson_get_int(mn) > 0)
                    r.min = (uint32_t)yyjson_get_int(mn);
                else if (mn && yyjson_is_num(mn) && yyjson_get_num(mn) >= 1.0)
                    r.min = (uint32_t)yyjson_get_num(mn);
                // else: absent / non-positive / wrong-type → default min = 1.
                out.push_back(std::move(r));
            }
        }
    }
    if (doc) yyjson_doc_free(doc);
    return out;
}

// ---------------------------------------------------------------------------
// SECURITY (P1) — path-containment guard for semi-trusted project-folder
// path strings.
//
// ROOT CAUSE: path strings read VERBATIM from project.json / plugin.json
// (`plugins[].path`, `script`, manifest `dll`) were joined onto a trusted base
// with std::filesystem::operator/ and used with NO containment check.
// operator/ with an ABSOLUTE right-hand side silently DISCARDS the base, and
// a "../" chain climbs out of it — so a project folder obtained from a
// semi-trusted source (unzipped customer project, network share, repo
// checkout) could address ANY path on the machine.
// THREAT: LoadLibrary of a PRE-PLANTED out-of-tree DLL (the only remaining
// gate is plugin_abi_compatible), or handing cl.exe an OUT-OF-TREE source
// file to compile and run.
//
// The guard: the candidate must be RELATIVE — no root directory and no root
// name, which also rejects drive-relative "C:foo" and UNC "//server/share"
// forms is_absolute() alone can miss on Windows — and
// (base / candidate).lexically_normal() must still start with
// base.lexically_normal(), i.e. no ".."-escape. Purely LEXICAL by design:
// it constrains what the untrusted STRING can address; symlink traversal is
// out of scope here (planting a symlink already requires machine access).
inline bool path_is_contained(const std::filesystem::path& base,
                              const std::filesystem::path& candidate) {
    if (candidate.is_absolute() ||
        candidate.has_root_name() || candidate.has_root_directory())
        return false;
    const auto b   = base.lexically_normal();
    const auto rel = (b / candidate).lexically_normal().lexically_relative(b);
    if (rel.empty()) return false;      // not expressible under base at all
    return *rel.begin() != "..";        // leading ".." = climbed out of base
}

// Companion (same P1): a plugin.json `dll` must be a BARE FILENAME, resolved
// inside the plugin's own folder. Any separator, ".." or drive prefix would
// let a semi-trusted manifest point the LoadLibrary target outside the plugin
// dir (e.g. "../../../Windows/Temp/evil.dll").
inline bool dll_name_is_bare(const std::string& n) {
    if (n.empty()) return false;
    if (n.find('/')  != std::string::npos ||
        n.find('\\') != std::string::npos ||
        n.find("..") != std::string::npos)
        return false;
    return !std::filesystem::path(n).has_root_name();   // "C:evil.dll"
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
    // SECURITY (P1): a manifest `dll` is used verbatim to build the
    // LoadLibrary target (<folder>/<dll_name> at every load/certify site), so
    // a separator / ".." / drive prefix escapes the plugin's own folder and
    // loads a pre-planted binary from anywhere. Require a bare filename;
    // otherwise warn loudly and INVALIDATE the plugin (empty name — the
    // established "invalid manifest" signal every parse_manifest caller
    // already skips on). See dll_name_is_bare / path_is_contained above.
    if (dll && !dll_name_is_bare(*dll)) {
        std::fprintf(stderr,
            "[xinsp2] plugin.json %s: \"dll\": \"%s\" is not a bare filename "
            "(contains a path separator, '..' or a drive prefix) — a manifest "
            "dll must name a file in the plugin's own folder. REJECTING "
            "plugin '%s' (path-containment guard).\n",
            path.c_str(), dll->c_str(), pi.name.c_str());
        pi.name.clear();   // invalid manifest — callers skip on empty name
        return pi;
    }
    if (dll)  pi.dll_name = *dll;
    else      pi.dll_name = pi.name + ".dll";
    // fs-aware platform module mapping at the single parse point, so every
    // downstream fs::exists / load / sha256 / cert-cache compare uses one
    // consistent name. Prefer the native module (xi-<name>.so/.dylib) ONLY when it
    // actually exists in the plugin folder: that maps a real prebuilt plugin
    // (built as .so) while leaving a fixture that ships a .dll-named module — or a
    // simply-unbuilt plugin — under its manifest name so the load site reports an
    // honest missing-module error. No-op on Windows (platform_module_path is
    // identity, so mapped == name and the branch is skipped).
    if (std::string mapped = platform_module_path(pi.dll_name);
        mapped != pi.dll_name &&
        std::filesystem::exists(std::filesystem::path(folder) / mapped))
        pi.dll_name = mapped;
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
    // Ordered output sink: `"sink": true` or `"role": "sink"`. The host stages +
    // gates use(<instance>).process() so the sink's side effect lands in frame order.
    pi.is_sink = json_flag_true(content, "sink") ||
                 (extract_string(content, "role").value_or("") == "sink");
    // doc 14 lib-plugin marker (informational) + V3 machine-autoload gate
    // (doc 19 V3): `"lib": true` flags a capability provider with no data plane;
    // `"autoload": true` makes the host instantiate it once at boot under a
    // machine owner so its capabilities are available without a per-project
    // instance (PluginManager::autoload_machine_providers). See PluginInfo.
    pi.is_lib   = json_flag_true(content, "lib");
    pi.autoload = json_flag_true(content, "autoload");
    // item 14: per-plugin post-fault policy DEFAULT ("on_fault"). An instance.json
    // "on_fault" overrides it per instance. Absent/unknown → Reuse (unchanged).
    pi.default_on_fault = parse_on_fault(extract_string(content, "on_fault").value_or(""),
                                         OnFault::Reuse);
    // LV2-style capability handshake: required interfaces gate the load; optional
    // ones are advisory (the plugin null-checks them at runtime). See parse_iface_reqs.
    pi.required_ifaces = parse_iface_reqs(content, "requires");
    pi.optional_ifaces = parse_iface_reqs(content, "optional");
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
