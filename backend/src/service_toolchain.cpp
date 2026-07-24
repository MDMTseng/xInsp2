//
// service_toolchain.cpp — C++ toolchain resolution + per-project override, script
// external-dep parsing, project DLL search path, and the instance crash-breadcrumb
// helpers. Split from service_main.cpp (behavior-preserving; see service_state.hpp / service_cmds.hpp).
//
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <yyjson.h>
#include <xi/xi_toolchain.hpp>
#include <xi/xi_project.hpp>

#include "service_cmds.hpp"

// ---- C++ toolchain health + per-project override -----------------------------
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
// The subsystem itself (component probing, override read/write, health JSON) is
// an UNRELATED concern lifted into xi/xi_toolchain.hpp (design review lens 2#6).
// It's pure functions of a project folder + the injected default include dir;
// service_main only owns the *global* resolved compiler paths (below) because the
// compile path consumes them, and copies xi::toolchain::resolve()'s result in.

// Apply a project's toolchain resolution to the global compiler paths. Called on
// open_project and after set_toolchain_override so the next compile + the
// generated IntelliSense config both pick up the override immediately.
void resolve_toolchain_(const std::string& folder) {
    auto r = xi::toolchain::resolve(folder, g_eng.include_dir_default);
    g_eng.include_dir    = r.include_dir;
    g_eng.opencv_dir     = r.opencv_dir;
    g_eng.turbojpeg_root = r.turbojpeg_root;
    g_eng.ipp_root       = r.ipp_root;
    g_eng.tc_vcvars      = r.vcvars;
    std::fprintf(stderr, "[xinsp2] toolchain resolved: opencv=%s turbojpeg=%s ipp=%s vcvars=%s\n",
                 g_eng.opencv_dir.empty() ? "none" : g_eng.opencv_dir.c_str(),
                 g_eng.turbojpeg_root.empty() ? "none" : g_eng.turbojpeg_root.c_str(),
                 g_eng.ipp_root.empty() ? "none" : g_eng.ipp_root.c_str(),
                 g_eng.tc_vcvars.empty() ? "auto" : g_eng.tc_vcvars.c_str());
}

// ---- script external dependencies (project.json include_dirs / link_libs) ----
//
// A user script (inspect.cpp) may need an external SDK. Unlike a plugin (which
// ships deps in its own folder), the script DLL lives in TEMP/script_build, so we
// give the script two project-level hooks:
//   "include_dirs": ["deps/include", "C:/abs/include"]  -> extra cl /I
//   "link_libs":    ["deps/foo.lib"]                     -> import libs to link
// Relative entries resolve against the project folder. The matching runtime DLL
// search of the project folder is set up in set_project_dll_search_ (below).
//
// NOTE: there is deliberately no "sources" (extra .cpp TUs) hook for scripts.
// The script-support thunks + xi::use()/VAR/state callback globals are bound
// per-TU (xi_use.hpp's `extern` resolves to xi_script_support.hpp's `static`
// only within the force-included primary TU), so a second .cpp TU can't call
// use()/VAR. Multi-file scripts split via headers #included into the one TU —
// see docs/guides/write-a-script.md. (Plugins, which export a C ABI, use
// extra_sources; scripts can't follow that model for this reason.)
void read_script_deps_(const std::string& folder,
                              std::vector<std::string>& include_dirs,
                              std::vector<std::string>& link_libs,
                              int& openmp_max_threads,
                              bool& allow_raw_omp) {
    if (folder.empty()) return;
    namespace fs = std::filesystem;
    std::ifstream in((fs::path(folder) / "project.json").string());
    if (!in) return;
    std::stringstream ss; ss << in.rdbuf();
    std::string src = ss.str();
    yyjson_doc* doc = yyjson_read(src.c_str(), src.size(), 0);
    if (!doc) return;
    yyjson_val* root = yyjson_doc_get_root(doc);
    auto resolve = [&](const char* s) -> std::string {
        fs::path p(s);
        if (p.is_absolute()) return p.string();
        std::error_code ec;
        return (fs::path(folder) / p).lexically_normal().string();
    };
    auto pull = [&](const char* key, std::vector<std::string>& out) {
        yyjson_val* arr = yyjson_obj_get(root, key);
        if (!arr || !yyjson_is_arr(arr)) return;
        size_t _i, _n; yyjson_val* it;
        yyjson_arr_foreach(arr, _i, _n, it) {
            const char* s = yyjson_get_str(it);
            if (yyjson_is_str(it) && s && *s)
                out.push_back(resolve(s));
        }
    };
    pull("include_dirs", include_dirs);
    pull("link_libs", link_libs);
    // Opt-in OpenMP for the script compile. 0/absent = off, N>0 = on capped to N,
    // -1 = on uncapped. Adds /openmp (+ cap macro) in the compiler.
    yyjson_val* omp = yyjson_obj_get(root, "openmp_max_threads");
    if (omp && yyjson_is_num(omp)) openmp_max_threads = yyjson_get_int(omp);
    // Blessed-concurrency opt-out (adoption map item 11): raw `#pragma omp` in a
    // script is rejected at compile time by default; an author who accepts the
    // worker-thread safety rules can set "allow_raw_omp": true to bypass the guard.
    yyjson_val* raw_omp = yyjson_obj_get(root, "allow_raw_omp");
    if (raw_omp && yyjson_is_bool(raw_omp)) allow_raw_omp = yyjson_get_bool(raw_omp);
    yyjson_doc_free(doc);
}

// Put the open project's folder on the process DLL search path so a script's
// statically-linked external dependency DLL can live in the project folder. The
// script loader (xi_script_loader.hpp) loads with LOAD_LIBRARY_SEARCH_USER_DIRS,
// which honours dirs added via AddDllDirectory. Re-pointed on each open_project.
// TODO(linux): equivalent is building the script .so with -Wl,-rpath plus
// dlopen; AddDllDirectory has no portable analogue.
void set_project_dll_search_(const std::string& folder) {
#ifdef _WIN32
    if (g_eng.proj_dll_dir) { RemoveDllDirectory(g_eng.proj_dll_dir); g_eng.proj_dll_dir = nullptr; }
    if (folder.empty()) return;
    int wn = MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), -1, nullptr, 0);
    if (wn <= 0) return;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), -1, w.data(), wn);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    g_eng.proj_dll_dir = AddDllDirectory(w.c_str());
#else
    // POSIX: no process-wide DLL-search-dir mechanism. A script's external .so
    // deps are resolved via the compiled module's RPATH/$ORIGIN and
    // LD_LIBRARY_PATH; this is intentionally a no-op. See TODO(linux) above.
    (void)folder;
#endif
}

// Plugin manager (global)

// (forward-declared above use_process_cb) record a per-instance process() crash.
void note_instance_crash_(const char* name, const char* why) {
    if (name) {
        g_eng.plugin_mgr.note_instance_crash(name, why ? why : "process() crashed");
        // Health contract: a caught process()/exchange() crash marks the instance
        // runtime-degraded (the quarantine seed, adoption-map item 14). This is the
        // FAULT path — rare, off the per-frame verdict path — so touching the
        // health mutex here is allowed. If the system is running this flips the
        // top-level state to `degraded` and emits health_changed.
        xi::health().mark_instance_fault(name, xi::CompHealth::Degraded,
                                         xi::kReasonPluginFault);
    }
}

// (forward-declared above use_process_inline_) Part III G2.1 culprit stamp.
// The plugin name -> {folder, dll} resolution needs the manager lock, so a
// thread_local cache resolves it ONCE per (plugin, thread) and re-resolves only
// when the active plugin changes — the per-frame process() hot path then costs
// just a string compare + the four strncpy inside set_culprit().
void stamp_culprit_(const char* instance, const std::string& plugin) {
    thread_local std::string t_plugin, t_folder, t_dll;
    if (plugin != t_plugin) {
        t_plugin = plugin;
        t_folder.clear();
        t_dll.clear();
        if (!plugin.empty())
            g_eng.plugin_mgr.plugin_location(plugin, t_folder, t_dll);  // lock only on change
    }
    xi::crash::set_culprit(instance ? instance : "", plugin.c_str(),
                           t_folder.c_str(), t_dll.c_str());
}
