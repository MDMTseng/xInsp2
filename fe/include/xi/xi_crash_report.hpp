#pragma once
//
// xi_crash_report.hpp — portable crash-report parser for the FE supervisor.
//
// The FE supervisor (fe_main.cpp) reads the backend's own log to find the
// absolute minidump path it printed on death, then reads the sibling .json
// crash report for forensics (exception name/module, last_phase) that it folds
// into the BeExitEvent (crash history / fe_status). That logic lived as a
// FILE-STATIC function inside fe_main.cpp (enrich_from_crash_report) and so
// could not be unit-tested in isolation.
//
// This header lifts it out verbatim (same regex, same last-match-wins, same
// threads[] fallback that prefers the "inspect" breadcrumb) into a portable,
// header-only function so it can be exercised by a C++ unit (test_qa_edge.cpp)
// against crafted be.log + .json fixtures.
//
// Deliberately portable: only <string>/<fstream>/<sstream>/<regex>/<filesystem>
// + yyjson (already a backend dep). No Win32, so it compiles unchanged on Linux
// (see docs/roadmap/linux-port.md). The FE simply forwards its &BeExitEvent.
//
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <yyjson.h>

#include <xi/xi_be_exit.hpp>

namespace xi {

// Parse the BE's log for the LAST "minidump: <path>.dmp" line it printed, then
// read the sibling .json crash report and fill ev.{report_path, exception_name,
// faulting_module, last_phase}. Best-effort and noexcept-by-contract: any
// missing/corrupt input leaves the corresponding fields untouched and returns
// without throwing.
//
//   - no be.log / unreadable          -> returns, fields untouched
//   - no "minidump:" line             -> returns, report_path untouched
//   - multiple "minidump:" lines      -> the LAST wins (most recent crash)
//   - sibling .json missing/corrupt   -> report_path set, other fields untouched
//   - context.last_phase empty        -> falls back to threads[]; prefers "inspect"
//
inline void enrich_from_crash_report(const std::string& be_log, BeExitEvent& ev) {
    std::ifstream f(be_log);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    std::string text = ss.str();

    // BE prints: "... — minidump: C:\...\xinsp-backend-<pid>-<ts>.dmp"
    std::regex re(R"(minidump:\s*(.+?\.dmp))");
    std::string dmp;
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
        dmp = (*it)[1].str();   // keep the last match
    if (dmp.empty()) return;
    // Trim trailing whitespace/CR.
    while (!dmp.empty() && (dmp.back() == '\r' || dmp.back() == '\n' || dmp.back() == ' '))
        dmp.pop_back();

    ev.dump_path = dmp;
    std::filesystem::path json_path =
        std::filesystem::path(dmp).replace_extension(".json");
    ev.report_path = json_path.string();
    std::ifstream jf(json_path);
    if (!jf) return;
    std::stringstream js; js << jf.rdbuf();
    std::string json_text = js.str();
    yyjson_doc* doc = yyjson_read(json_text.c_str(), json_text.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!root) { yyjson_doc_free(doc); return; }
    if (yyjson_val* exc = yyjson_obj_get(root, "exception")) {
        if (yyjson_val* nm = yyjson_obj_get(exc, "name"); yyjson_is_str(nm))
            ev.exception_name = yyjson_get_str(nm);
        if (yyjson_val* md = yyjson_obj_get(exc, "module"); yyjson_is_str(md))
            ev.faulting_module = yyjson_get_str(md);
    }
    if (yyjson_val* ctx = yyjson_obj_get(root, "context")) {
        if (yyjson_val* ph = yyjson_obj_get(ctx, "last_phase"); yyjson_is_str(ph))
            ev.last_phase = yyjson_get_str(ph);
    }
    // Part III G2.1 — per-plugin attribution. The backend stamps a process-global
    // `culprit` {instance, plugin, folder, dll} = the plugin it last entered. We
    // attribute the death to that plugin ONLY when the faulting MODULE (the DLL the
    // instruction pointer was actually in, from blame_module) matches the culprit's
    // dll — so a crash in the script or the core itself is never mis-blamed on the
    // last plugin the host happened to touch. When they match we also carry the
    // folder + dll forward so the supervisor can quarantine it via G1's mechanism.
    if (yyjson_val* cul = yyjson_obj_get(root, "culprit")) {
        auto cstr = [&](const char* k) -> std::string {
            yyjson_val* v = yyjson_obj_get(cul, k);
            const char* s = yyjson_get_str(v);
            return (yyjson_is_str(v) && s) ? std::string(s) : std::string();
        };
        std::string c_plugin   = cstr("plugin");
        std::string c_instance = cstr("instance");
        std::string c_folder   = cstr("folder");
        std::string c_dll      = cstr("dll");
        // faulting_module is "<module>+0x<off>" (or "<unknown>"), where <module>
        // today is just the DLL BASENAME (crash::blame_module strips the
        // directory). ROOT CAUSE of the mis-attribution bug: under the standard
        // build convention many plugins share the SAME basename (plugin_vN.dll —
        // real collisions exist across examples/circle_counting), and g_culprit
        // is a single last-writer process-global stamp, so a crash in plugin A
        // can carry same-basename plugin B's identity. Matching here therefore:
        //   1. prefers a FULL-PATH match whenever the report's module carries a
        //      directory (unambiguous — compares against culprit folder + dll);
        //   2. otherwise requires EXACT basename equality (the old substring
        //      find() also matched suffixes, e.g. culprit dll "plugin.dll"
        //      inside "myplugin.dll+0x...").
        // A basename-only match remains inherently ambiguous when >=2 loaded
        // plugins share that basename; the FE supervisor (fe_main.cpp) detects
        // that collision and then refuses to quarantine / reset the respawn
        // budget. FULL FIX direction: record + match the full module path
        // end-to-end (blame_module keeps GetModuleFileNameA's absolute path,
        // the report stores it, and branch 1 below then always applies).
        auto path_ieq = [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                char x = a[i], y = b[i];
                if (x >= 'A' && x <= 'Z') x += 32;   // Windows paths: case-insensitive
                if (y >= 'A' && y <= 'Z') y += 32;
                if (x == '\\') x = '/';              // separators equivalent
                if (y == '\\') y = '/';
                if (x != y) return false;
            }
            return true;
        };
        std::string mod = ev.faulting_module.substr(0, ev.faulting_module.find('+'));
        size_t      sep = mod.find_last_of("/\\");
        bool mod_has_path = sep != std::string::npos;
        std::string mod_base = mod_has_path ? mod.substr(sep + 1) : mod;
        bool dll_matches = false;
        if (!c_dll.empty() && !mod.empty() && mod != "<unknown>") {
            if (mod_has_path && !c_folder.empty()) {
                // Full module path available: disambiguate by the whole path.
                std::string full = (std::filesystem::path(c_folder) / c_dll)
                                       .lexically_normal().string();
                dll_matches = path_ieq(
                    std::filesystem::path(mod).lexically_normal().string(), full);
            } else {
                // Basename-only report: exact (not substring) basename equality.
                dll_matches = path_ieq(mod_base, c_dll);
            }
        }
        if (dll_matches) {
            ev.culprit_plugin   = c_plugin;
            ev.culprit_instance = c_instance;
            ev.culprit_folder   = c_folder;
            ev.culprit_dll      = c_dll;
        }
    }
    // If context didn't name a phase (crash on an unmanaged thread), fall back
    // to the most informative thread breadcrumb in threads[].
    if (ev.last_phase.empty()) {
        if (yyjson_val* th = yyjson_obj_get(root, "threads"); yyjson_is_arr(th)) {
            size_t _i, _n; yyjson_val* t;
            yyjson_arr_foreach(th, _i, _n, t) {
                yyjson_val* ph = yyjson_obj_get(t, "last_phase");
                const char* phs = yyjson_get_str(ph);
                if (yyjson_is_str(ph) && phs && phs[0]) {
                    ev.last_phase = phs;
                    if (std::string(phs) == "inspect") break;  // prefer inspect
                }
            }
        }
    }
    yyjson_doc_free(doc);
}

} // namespace xi
