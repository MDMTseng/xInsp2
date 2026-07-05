#pragma once
//
// xi_toolchain.hpp — C++ toolchain health + per-project override resolution.
//
// Extracted from service_main.cpp (design review lens 2#6): the VS toolchain /
// vcvars / OpenCV / IPP / libjpeg-turbo discovery + the project.json "toolchain"
// override-block handling is a self-contained subsystem with ZERO relationship
// to WS dispatch or inspection — it lived in service_main only because main()
// needed it. These are pure, stateless functions of a project folder (plus the
// backend's default xi-include dir, which is INJECTED, never a global here).
//
// A project may pin toolchain paths in its project.json "toolchain" block:
//   "toolchain": {
//     "include_dir":     "...",   // xi headers (defaults to the shipped set)
//     "opencv_dir":      "...",   // OpenCV install root
//     "turbojpeg_root":  "...",   // libjpeg-turbo root (optional accelerator)
//     "ipp_root":        "...",   // Intel IPP root      (optional accelerator)
//     "vcvars":          "..."    // path to vcvars64.bat (else auto-found)
//   }
// Resolution priority per component: project override > env var > built-in probe
// (which itself checks env then default candidates). This lets a user fix a
// wrong/missing path from the VS Code config UI without touching global
// environment. The same resolved values feed the compiler AND (via
// cmd:toolchain_health) the extension's c_cpp_properties.json, so IntelliSense
// can't drift from the build.
//
// service_main owns the *global* resolved compiler paths (g_include_dir,
// g_opencv_dir, ...) because the compile path consumes them; it calls resolve()
// here to compute the values and copies them into those globals. The WS layer
// (toolchain_health / set_toolchain_override) calls health_json() / write_override().
//
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <yyjson.h>

#include "xi_atomic_io.hpp"       // xi::atomic_write (torn-write-safe project.json write)
#include "xi_json_escape.hpp"     // xi::json_escape_into
#include "xi_script_compiler.hpp" // xi::script::detail::probe_* / auto_find_vcvars

namespace xi {
namespace toolchain {

// One toolchain component (include headers / OpenCV / turbojpeg / ipp / vcvars).
struct Component {
    std::string key;       // stable id: "include" | "opencv" | "turbojpeg" | "ipp" | "vcvars"
    std::string label;     // human label
    std::string ov_key;    // project.json toolchain field name
    std::string env_var;   // env var that also sets it ("" = none)
    std::string sentinel;  // relative file proving the dir is real ("" = path is a file)
    std::string path;      // resolved path (may be empty)
    std::string source;    // "override" | "env" | "default" | "none"
    bool exists = false;   // sentinel (or the file itself, for vcvars) present
    bool optional = false; // optional accelerator → missing is info, not error
};

// Resolved compiler paths that service_main copies into its global compile state.
struct Resolved {
    std::string include_dir;    // xi headers (override path, else the injected default)
    std::string opencv_dir;
    std::string turbojpeg_root;
    std::string ipp_root;
    std::string vcvars;         // override path only, else empty (compiler auto-finds)
};

// Read one string field from the "toolchain" object of <folder>/project.json.
inline std::string read_override(const std::string& folder, const char* field) {
    if (folder.empty()) return {};
    std::ifstream in((std::filesystem::path(folder) / "project.json").string());
    if (!in) return {};
    std::stringstream ss; ss << in.rdbuf();
    std::string out;
    std::string s = ss.str();
    if (yyjson_doc* doc = yyjson_read(s.c_str(), s.size(), 0)) {
        yyjson_val* root = yyjson_doc_get_root(doc);
        if (yyjson_val* tc = yyjson_obj_get(root, "toolchain"); tc && yyjson_is_obj(tc))
            if (yyjson_val* k = yyjson_obj_get(tc, field); k && yyjson_is_str(k) && yyjson_get_str(k))
                out = yyjson_get_str(k);
        yyjson_doc_free(doc);
    }
    return out;
}

inline bool sentinel_ok(const Component& c) {
    if (c.path.empty()) return false;
    std::error_code ec;
    if (c.sentinel.empty())  // vcvars: the path IS the file
        return std::filesystem::exists(c.path, ec);
    return std::filesystem::exists(std::filesystem::path(c.path) / c.sentinel, ec);
}

// Build the live component list for `folder` (the open project; "" = no project,
// startup defaults only). `path` comes straight from override/probe so it's always
// accurate; `source` is best-effort labelling for the UI. `include_dir_default` is
// the backend's shipped xi-include dir (service_main derives it at startup).
inline std::vector<Component> resolve_components(const std::string& folder,
                                                 const std::string& include_dir_default) {
    using namespace xi::script::detail;
    std::vector<Component> v(5);
    v[0] = { "include",   "xi headers",        "include_dir",    "",               "xi/xi.hpp",                  "", "", false, false };
    v[1] = { "opencv",    "OpenCV",            "opencv_dir",     "OpenCV_DIR",     "include/opencv2/core.hpp",   "", "", false, false };
    v[2] = { "turbojpeg", "libjpeg-turbo",     "turbojpeg_root", "TURBOJPEG_ROOT", "include/turbojpeg.h",        "", "", false, true  };
    v[3] = { "ipp",       "Intel IPP",         "ipp_root",       "IPP_ROOT",       "include/ippi.h",             "", "", false, true  };
    v[4] = { "vcvars",    "MSVC (vcvars64)",   "vcvars",         "",               "",                           "", "", false, false };

    for (auto& c : v) {
        std::string ov = read_override(folder, c.ov_key.c_str());
        if (!ov.empty()) { c.path = ov; c.source = "override"; }
        else {
            // Built-in probe (already honours env then default candidates).
            std::string probed;
            if      (c.key == "include")   probed = include_dir_default;
            else if (c.key == "opencv")    probed = probe_opencv_dir();
            else if (c.key == "turbojpeg") probed = probe_turbojpeg_root();
            else if (c.key == "ipp")       probed = probe_ipp_root();
            else if (c.key == "vcvars")    probed = auto_find_vcvars();
            c.path = probed;
            const char* e = c.env_var.empty() ? nullptr : std::getenv(c.env_var.c_str());
            if (!c.path.empty() && e && *e)   c.source = "env";
            else if (!c.path.empty())         c.source = "default";
            else                              c.source = "none";
        }
        c.exists = sentinel_ok(c);
    }
    return v;
}

// Compute the resolved compiler paths for `folder`. service_main copies the
// result into its global compile state (and logs) on open_project /
// set_toolchain_override so the next compile + the IntelliSense config both pick
// up the override immediately.
inline Resolved resolve(const std::string& folder, const std::string& include_dir_default) {
    auto comps = resolve_components(folder, include_dir_default);
    Resolved r;
    for (auto& c : comps) {
        if      (c.key == "include")   r.include_dir    = (c.source == "override") ? c.path : include_dir_default;
        else if (c.key == "opencv")    r.opencv_dir     = c.path;
        else if (c.key == "turbojpeg") r.turbojpeg_root = c.path;
        else if (c.key == "ipp")       r.ipp_root       = c.path;
        else if (c.key == "vcvars")    r.vcvars         = (c.source == "override") ? c.path : std::string();
    }
    return r;
}

// Render the health report as JSON for the toolchain_health command / UI.
inline std::string health_json(const std::string& folder, const std::string& include_dir_default) {
    auto comps = resolve_components(folder, include_dir_default);
    bool all_ok = true;
    std::string out = "{\"components\":[";
    for (size_t i = 0; i < comps.size(); ++i) {
        auto& c = comps[i];
        // ok rules: an explicit override that doesn't resolve is always an error
        // (the user pointed us somewhere wrong); a required component must exist;
        // an optional one that's simply absent is fine.
        bool ok;
        if (c.source == "override") ok = c.exists;
        else if (!c.optional)       ok = c.exists;
        else                        ok = true;
        if (!ok) all_ok = false;

        std::string hint;
        if (c.source == "override" && !c.exists)
            hint = c.sentinel.empty() ? "overridden path does not exist"
                                      : ("expected " + c.sentinel + " under this folder");
        else if (!c.exists && !c.optional)
            hint = c.key == "vcvars" ? "vcvars64.bat not found — install VS Build Tools (Desktop C++ workload)"
                                     : ("not found — set " + (c.env_var.empty() ? std::string("an override") : c.env_var) + " or fix the path");
        else if (!c.exists && c.optional)
            hint = "optional accelerator, not installed";

        if (i) out += ",";
        out += "{\"key\":";       xi::json_escape_into(out, c.key);
        out += ",\"label\":";     xi::json_escape_into(out, c.label);
        out += ",\"path\":";      xi::json_escape_into(out, c.path);
        out += ",\"source\":";    xi::json_escape_into(out, c.source);
        out += ",\"env_var\":";   xi::json_escape_into(out, c.env_var);
        out += ",\"ov_key\":";    xi::json_escape_into(out, c.ov_key);
        out += ",\"exists\":";    out += c.exists ? "true" : "false";
        out += ",\"optional\":";  out += c.optional ? "true" : "false";
        out += ",\"ok\":";        out += ok ? "true" : "false";
        out += ",\"hint\":";      xi::json_escape_into(out, hint);
        out += "}";
    }
    out += "],\"all_ok\":";
    out += all_ok ? "true" : "false";
    out += ",\"project\":";
    xi::json_escape_into(out, folder);
    out += "}";
    return out;
}

// Merge one override into the canonical project.json "toolchain" block. Empty
// value clears that key (revert to env/probe). Returns false (with `err`) if the
// project.json can't be read/parsed/written.
// NOTE (RT8 latent): this rewrites project.json WITHOUT the PluginManager mu_ that
// save_project_locked takes. Safe ONLY because every WS command handler runs serially
// on the single poll thread, so this can't interleave with save_project_locked. If
// command handling ever becomes multi-threaded, this must take mu_ too or it will tear
// project.json against a concurrent save.
inline bool write_override(const std::string& folder, const std::string& field,
                           const std::string& value, std::string& err) {
    namespace fs = std::filesystem;
    if (folder.empty()) { err = "no project open"; return false; }
    fs::path pj = fs::path(folder) / "project.json";
    std::ifstream in(pj.string());
    if (!in) { err = "cannot read project.json"; return false; }
    std::stringstream ss; ss << in.rdbuf();
    in.close();
    std::string src = ss.str();
    yyjson_doc* idoc = yyjson_read(src.c_str(), src.size(), 0);
    if (!idoc) { err = "project.json is not valid JSON"; return false; }
    // yyjson read DOM is immutable — copy to a mutable doc to edit in place.
    yyjson_mut_doc* d = yyjson_doc_mut_copy(idoc, NULL);
    yyjson_doc_free(idoc);
    if (!d) { err = "project.json is not valid JSON"; return false; }
    yyjson_mut_val* root = yyjson_mut_doc_get_root(d);
    yyjson_mut_val* tc = root ? yyjson_mut_obj_get(root, "toolchain") : nullptr;
    if (!tc || !yyjson_mut_is_obj(tc)) {
        yyjson_mut_obj_remove_str(root, "toolchain");  // drop any non-object
        tc = yyjson_mut_obj_add_obj(d, root, "toolchain");
    }
    if (value.empty()) yyjson_mut_obj_remove_str(tc, field.c_str());
    else {
        yyjson_mut_obj_remove_str(tc, field.c_str());
        yyjson_mut_obj_add_strcpy(d, tc, field.c_str(), value.c_str());
    }
    // Drop an emptied toolchain object so we don't leave "toolchain":{} noise.
    if (yyjson_mut_obj_size(tc) == 0) yyjson_mut_obj_remove_str(root, "toolchain");
    char* printed = yyjson_mut_write(d, YYJSON_WRITE_PRETTY, NULL);
    bool ok = false;
    if (printed) {
        // Route through atomic_write: a torn write here truncates the canonical
        // project.json (whole-project config loss). atomic_write leaves the prior
        // file intact on any failure and only renames the complete new content.
        if (xi::atomic_write(pj, std::string(printed) + "\n")) ok = true;
        else err = "cannot write project.json";
        free(printed);
    } else err = "failed to serialize project.json";
    yyjson_mut_doc_free(d);
    return ok;
}

} // namespace toolchain
} // namespace xi
