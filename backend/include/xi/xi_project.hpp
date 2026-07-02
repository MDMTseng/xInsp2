#pragma once
//
// xi_project.hpp — project.json save/load for xInsp2.
//
// A project file stores at minimum:
//   - a schema identity   ("schema": "xi.project/1")
//   - param values        ("params")
//   - instance defs        ("instances")
// plus keys owned by the backend (parallelism / runtime / groups / plugin_dirs /
// plugins / toolchain / include_dirs / link_libs) and keys another writer owns
// (the VS Code extension's params / auto_respawn / watchdog_ms). It is a
// multi-key, multi-writer document.
//
// Format is a plain JSON object, e.g.:
//
//   {
//     "schema": "xi.project/1",
//     "params": { "sigma": 3.5, "low": 60 },
//     "instances": { "cam0": { ... }, "template": { ... } }
//   }
//
// PERSISTENCE CONTRACT (Finding 1 / adoption item 12): a save is a
// read–modify–write over the WHOLE existing document — it overwrites ONLY the
// keys the saver owns and preserves every other top-level key verbatim. There is
// deliberately no helper that rebuilds project.json from partial knowledge; a
// prior `build_project_json(params, instances)` did exactly that and silently
// dropped every other key (runtime / parallelism / groups / the extension's
// keys) on a normal save. Use merge_project_json instead.
//

#include "xi_atomic_io.hpp"

#include <yyjson.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace xi::project {

// --- Schema identity ------------------------------------------------------
// Matches the run-outcome schema's naming style ("xi.run-outcome/1"): a dotted
// family plus a MAJOR version after the slash. The major is bumped only on a
// breaking project-file format change; within a major, readers stay forgiving
// (unknown keys tolerated, missing keys defaulted).
constexpr const char* kProjectSchemaFamily = "xi.project";
constexpr int         kProjectSchemaMajor  = 1;
constexpr const char* kProjectSchema       = "xi.project/1";

// Parse a "<family>/<major>" schema string. Returns true and fills `major_out`
// only when the family matches kProjectSchemaFamily and the tail is a
// non-empty run of digits; false for any other string (a foreign family, a
// missing/empty major, non-numeric junk).
inline bool parse_project_schema(const std::string& s, int& major_out) {
    const std::string prefix = std::string(kProjectSchemaFamily) + "/";
    if (s.size() <= prefix.size() || s.compare(0, prefix.size(), prefix) != 0)
        return false;
    long m = 0;
    for (size_t i = prefix.size(); i < s.size(); ++i) {
        char c = s[i];
        if (c < '0' || c > '9') return false;
        m = m * 10 + (c - '0');
        if (m > 1'000'000) return false;   // absurd — treat as unrecognized
    }
    major_out = (int)m;
    return true;
}

inline std::string read_text(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline bool write_text(const std::string& path, const std::string& content) {
    return xi::atomic_write(std::filesystem::path(path), content);
}

// Read–modify–write merge for the `save_project` command.
//
// `existing` is the current on-disk project.json (may be empty / absent /
// unparseable). `params_json` and `instances_json` are the raw JSON the saver
// OWNS (the script's `xi_script_list_params` / `xi_script_list_instances`
// output). The result is a full document that:
//   - stamps the current "schema",
//   - sets "params" and "instances" to the freshly-provided values, and
//   - carries EVERY other top-level key from `existing` verbatim
//     (runtime / parallelism / groups / plugin_dirs / plugins / toolchain /
//      include_dirs / link_libs, and foreign keys like auto_respawn /
//      watchdog_ms).
// Nothing is reconstructed from partial knowledge — an unrecognized key is
// preserved, never dropped. If `existing` is empty/unparseable it is treated as
// an absent document and only schema/params/instances are written (no bytes to
// preserve), matching the create-new-file case.
inline std::string merge_project_json(const std::string& existing,
                                      const std::string& params_json,
                                      const std::string& instances_json) {
    yyjson_mut_doc* md = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(md);
    yyjson_mut_doc_set_root(md, root);

    // schema identity first.
    yyjson_mut_obj_add_str(md, root, "schema", kProjectSchema);

    // The keys this saver OWNS — parsed from the script thunks (each a JSON
    // array/object string). An empty/unparseable owned value falls back to an
    // empty array so the document is always well-formed.
    auto add_owned = [&](const char* key, const std::string& js) {
        yyjson_doc* d = js.empty() ? nullptr : yyjson_read(js.data(), js.size(), 0);
        yyjson_val* r = d ? yyjson_doc_get_root(d) : nullptr;
        yyjson_mut_val* mk = yyjson_mut_strcpy(md, key);
        yyjson_mut_val* mv = r ? yyjson_val_mut_copy(md, r) : yyjson_mut_arr(md);
        if (mk && mv) yyjson_mut_obj_add(root, mk, mv);
        if (d) yyjson_doc_free(d);
    };
    add_owned("params", params_json);
    add_owned("instances", instances_json);

    // Preserve every OTHER top-level key from the existing document verbatim.
    if (!existing.empty()) {
        if (yyjson_doc* od = yyjson_read(existing.data(), existing.size(), 0)) {
            yyjson_val* oroot = yyjson_doc_get_root(od);
            if (yyjson_is_obj(oroot)) {
                size_t idx, mx; yyjson_val *k, *v;
                yyjson_obj_foreach(oroot, idx, mx, k, v) {
                    const char* key = yyjson_get_str(k);
                    if (!key) continue;
                    // schema/params/instances were just written fresh — skip the
                    // old copies so the new values win.
                    if (!std::strcmp(key, "schema") ||
                        !std::strcmp(key, "params") ||
                        !std::strcmp(key, "instances")) continue;
                    yyjson_mut_val* mk = yyjson_mut_strcpy(md, key);
                    yyjson_mut_val* mv = yyjson_val_mut_copy(md, v);
                    if (mk && mv) yyjson_mut_obj_add(root, mk, mv);
                }
            }
            yyjson_doc_free(od);
        }
    }

    std::string out;
    if (char* w = yyjson_mut_write(md, YYJSON_WRITE_PRETTY, nullptr)) {
        out.assign(w);
        free(w);
    }
    yyjson_mut_doc_free(md);
    if (out.empty()) out = "{}\n";   // never write an empty file
    return out;
}

} // namespace xi::project
