//
// service_cmd_plugin.cpp — plugin-mgmt command handlers, split from
// service_main.cpp (behavior-preserving; see service_internal.hpp).
//
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <yyjson.h>
#include <xi/xi_toolchain.hpp>

#include "service_internal.hpp"

// ---- plugin-mgmt -----------------------------------------------------------
void cmd_set_process_priority_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Live process priority (Win). class: high|above|normal|below|realtime.
        // Mirrors --priority / project.json runtime.process_priority.
        auto c = xp::get_string_field(parsed->args_json, "class");
        std::string cls = c ? *c : "";
        if (apply_process_priority_(cls)) {
            send_rsp_ok(srv, id, "{\"process_priority\":\"" + cls + "\"}");
        } else {
            send_rsp_err(srv, id, "bad priority class (high|above|normal|below|realtime)");
        }
}

void cmd_list_plugins_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        auto plugins = g_eng.plugin_mgr.list_plugins();
        std::string out = "[";
        auto esc = [](const std::string& s) {
            std::string o; for (char c : s) { if (c=='\\'||c=='"') o.push_back('\\'); o.push_back(c); } return o;
        };
        for (size_t i = 0; i < plugins.size(); ++i) {
            if (i) out += ",";
            auto& p = plugins[i];
            out += "{\"name\":\"" + esc(p.name) + "\",\"description\":\"" + esc(p.description) + "\"";
            out += ",\"folder\":\"" + esc(p.folder_path) + "\"";
            out += ",\"has_ui\":" + std::string(p.has_ui ? "true" : "false");
            out += ",\"loaded\":" + std::string(p.handle ? "true" : "false");
            // cmake/prebuilt plugins get the per-item "Rebuild" action in the
            // extension's Plugin Browser (rebuild_plugins {plugins:[name]}).
            out += ",\"prebuilt\":" + std::string(p.prebuilt ? "true" : "false");
            // Same origin field as to_json — the extension's Plugin Browser relies
            // on it to badge project plugins, e2e journey asserts it.
            bool is_proj = g_eng.plugin_mgr.is_project_plugin(p.name);
            out += ",\"origin\":\"" + std::string(is_proj ? "project" : "global") + "\"";
            // Optional `manifest` block from plugin.json (free-form;
            // see docs/reference/c-abi.md). AI agents and doc
            // tools read this to discover params / inputs / outputs /
            // exchange surface without grepping plugin source.
            if (!p.manifest_json.empty()) {
                out += ",\"manifest\":" + p.manifest_json;
            }
            out += "}";
        }
        out += "]";
        send_rsp_ok(srv, id, out);
}

void cmd_rescan_plugins_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Optional arg: {"dir": "<path>"} scans that one dir (additive).
        // No arg: re-scan the default plugins_dir.
        auto dir_opt = xp::get_string_field(parsed->args_json, "dir");
        const std::string& dir = dir_opt ? *dir_opt : g_eng.plugins_dir;
        int n = 0;
        if (!dir.empty() && std::filesystem::exists(dir)) {
            // RT5/J2: scan_plugins' "moved" branch FreeLibrary's a plugin DLL that a
            // live CAbiInstanceAdapter may still hold (its dll_ + destroy_fn_) — the
            // same un-quiesced DLL-teardown class as remove_instance. Quiesce dispatch
            // for the scan so no worker is mid-call into an about-to-be-unmapped
            // instance. (Also bounds J6: dispatch is paused, so holding mu_ across a
            // certify subprocess can't stall the emit hot path.)
            auto _rescan_guard = quiesce_dispatch_for_lifecycle_op_("rescan_plugins", &srv);
            n = g_eng.plugin_mgr.scan_plugins(_rescan_guard.token(), dir);
        }
        std::string out = "{\"scanned\":";
        xp::json_escape_into(out, dir);
        out += ",\"count\":" + std::to_string(n) + "}";
        send_rsp_ok(srv, id, out);
}

/* [cmd_unquarantine_plugin_ and cmd_load_plugin_ RETIRED at THE CUT (v12) —
 * app-team confirmed, doc 11. Zero in-tree callers; rescan_plugins re-arms a
 * cleaned plugin, and instances load their plugins on project open. The
 * PluginManager::unquarantine_plugin/load_plugin methods remain for internal
 * use; only the WS command surface is retired.] */

void cmd_export_project_plugin_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Package a project plugin as a deployable folder. Compiles Release;
        // the destination contains a self-contained plugin.json + DLL that can
        // be dropped into another project's plugins folder.
        auto pname = xp::get_string_field(parsed->args_json, "plugin");
        auto dest  = xp::get_string_field(parsed->args_json, "dest");
        if (!pname || !dest) { send_rsp_err(srv, id, "missing plugin or dest"); return; }
        if (!g_eng.plugin_mgr.is_project_plugin(*pname)) {
            send_rsp_err(srv, id, "not a project plugin: " + *pname);
            return;
        }
        // export_project_plugin recompiles in Release; quiesce so no dispatcher
        // worker is mid-call into the same plugin's instances.
        auto _export_guard = quiesce_dispatch_for_lifecycle_op_("export_project_plugin", &srv);  // resumes at block end
        auto er = g_eng.plugin_mgr.export_project_plugin(_export_guard.token(), *pname, *dest);
        std::string data = "{\"plugin\":";
        xp::json_escape_into(data, *pname);
        data += ",\"dest\":";
        xp::json_escape_into(data, er.dest_dir);
        data += "}";
        if (er.ok) {
            send_rsp_ok(srv, id, data);
        } else {
            xp::Rsp r;
            r.id = id;
            r.ok = false;
            r.error = er.error;
            r.data_json = data;
            srv.send_text(r.to_json());
            if (!er.build_log.empty()) {
                xp::LogMsg lm;
                lm.level = "error";
                lm.msg = er.build_log;
                srv.send_text(lm.to_json());
            }
        }
}

void cmd_recompile_project_plugin_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Hot-rebuild a single project-local plugin. The extension calls
        // this from a file watcher when the user edits plugin source.
        // On success the plugin's instances are re-instantiated with
        // their previous defs intact; on failure the old DLL stays
        // loaded so running inspection isn't disrupted.
        auto pname = xp::get_string_field(parsed->args_json, "plugin");
        if (!pname) { send_rsp_err(srv, id, "missing plugin"); return; }
        if (!g_eng.plugin_mgr.is_project_plugin(*pname)) {
            send_rsp_err(srv, id, "not a project plugin: " + *pname);
            return;
        }
        // P0-AB-4: recompile resets each instance pointer then
        // FreeLibrary's the old DLL. Any in-flight set_def / exchange
        // on those instances from a dispatcher worker would dereference
        // freed code. Drain first.
        auto guard = quiesce_dispatch_for_lifecycle_op_("recompile_project_plugin", &srv);
        auto rr = g_eng.plugin_mgr.recompile_project_plugin(guard.token(), *pname);
        // Build diagnostics JSON — same shape as compile_and_load.
        std::string diag_json = "[";
        for (size_t i = 0; i < rr.diagnostics.size(); ++i) {
            auto& d = rr.diagnostics[i];
            if (i) diag_json += ",";
            diag_json += "{\"file\":";  xp::json_escape_into(diag_json, d.file);
            diag_json += ",\"line\":" + std::to_string(d.line);
            diag_json += ",\"col\":"  + std::to_string(d.col);
            diag_json += ",\"severity\":"; xp::json_escape_into(diag_json, d.severity);
            diag_json += ",\"code\":";    xp::json_escape_into(diag_json, d.code);
            diag_json += ",\"message\":"; xp::json_escape_into(diag_json, d.message);
            diag_json += "}";
        }
        diag_json += "]";
        std::string data = "{\"plugin\":";
        xp::json_escape_into(data, *pname);
        data += ",\"diagnostics\":" + diag_json;
        data += ",\"reattached\":[";
        for (size_t i = 0; i < rr.reattached_instances.size(); ++i) {
            if (i) data += ",";
            xp::json_escape_into(data, rr.reattached_instances[i]);
        }
        data += "]}";
        if (rr.ok) {
            send_rsp_ok(srv, id, data);
        } else {
            xp::Rsp r;
            r.id = id;
            r.ok = false;
            r.error = rr.error;
            r.data_json = data;
            srv.send_text(r.to_json());
            if (!rr.build_log.empty()) {
                xp::LogMsg lm;
                lm.level = "error";
                lm.msg = rr.build_log;
                srv.send_text(lm.to_json());
            }
        }
}

void cmd_rebuild_plugins_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // `xInsp2: Rebuild Plugins`. For every cmake/prebuilt plugin whose source
        // changed, the backend unloads it (releasing the DLL file lock), runs its
        // own CMake build, then loads the rebuilt DLL and restores each instance's
        // def. Unchanged plugins (incl. CUDA/heavy-state ones you didn't touch)
        // are skipped. The unload→build→load ordering is why CMake runs host-side
        // (Windows can't overwrite a loaded DLL; CMake emits a fixed-name DLL).
        //
        // Optional args: {"cmake":"<path>", "config":"Release",
        //                 "plugins":["a","b"]}. `plugins` restricts the rebuild to
        // those names (the extension passes it to rebuild just what you're editing);
        // omitted = every cmake plugin.
        //
        // Same quiesce constraint as recompile: this resets instance pointers and
        // FreeLibrary's DLLs — drain dispatch first.
        auto cmake_exe = xp::get_string_field(parsed->args_json, "cmake");
        auto config    = xp::get_string_field(parsed->args_json, "config");
        std::vector<std::string> only;
        if (yyjson_doc* adoc = yyjson_read(parsed->args_json.c_str(), parsed->args_json.size(), 0)) {
            yyjson_val* arr = yyjson_obj_get(yyjson_doc_get_root(adoc), "plugins");
            if (yyjson_is_arr(arr)) {
                size_t _i, _n; yyjson_val* it;
                yyjson_arr_foreach(arr, _i, _n, it) {
                    const char* s = yyjson_get_str(it);
                    if (yyjson_is_str(it) && s) only.emplace_back(s);
                }
            }
            yyjson_doc_free(adoc);
        }
        auto guard = quiesce_dispatch_for_lifecycle_op_("rebuild_plugins", &srv);
        auto rep = g_eng.plugin_mgr.rebuild_cmake_plugins(
            guard.token(),
            cmake_exe ? *cmake_exe : std::string("cmake"),
            config    ? *config    : std::string("Release"),
            only);
        std::string data = "{\"plugins\":[";
        bool any_fail = false;
        for (size_t i = 0; i < rep.items.size(); ++i) {
            auto& it = rep.items[i];
            if (i) data += ",";
            data += "{\"plugin\":"; xp::json_escape_into(data, it.name);
            data += ",\"status\":"; xp::json_escape_into(data, it.status);
            data += ",\"detail\":"; xp::json_escape_into(data, it.detail);
            data += "}";
            if (it.status == "failed") any_fail = true;
        }
        data += "]}";
        // Partial failures (failed[]) are still a completed run — return ok with
        // the per-plugin report; the client surfaces failures.
        send_rsp_ok(srv, id, data);
        if (any_fail)
            for (auto& it : rep.items)
                if (it.status == "failed") {
                    xp::LogMsg lm; lm.level = "error";
                    lm.msg = "rebuild_plugins: " + it.name + ": " + it.detail;
                    srv.send_text(lm.to_json());
                }
}

void cmd_get_plugin_ui_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Return the path to the plugin's UI folder so the extension can
        // load it into a webview.
        auto plugin = xp::get_string_field(parsed->args_json, "plugin");
        if (!plugin) { send_rsp_err(srv, id, "missing plugin"); return; }
        auto* pi = g_eng.plugin_mgr.find_plugin(*plugin);
        if (pi && pi->has_ui) {
            std::string data = "{\"ui_path\":";
            xp::json_escape_into(data, pi->ui_path);
            data += "}";
            send_rsp_ok(srv, id, data);
        } else {
            send_rsp_err(srv, id, "no UI for plugin: " + *plugin);
        }
}

void cmd_toolchain_health_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // C++ toolchain health check for the open project. Reports each
        // component's resolved path + source (override/env/default/none) +
        // whether it exists, so the config UI can warn on missing/wrong paths.
        send_rsp_ok(srv, id, xi::toolchain::health_json(g_eng.project_folder, g_eng.include_dir_default));
}

void cmd_set_toolchain_override_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Pin (or clear) one toolchain path in the project's project.json
        // "toolchain" block. args: { key: "opencv"|"turbojpeg"|"ipp"|"vcvars"|
        // "include", path: "<dir-or-file>" }  (empty path clears the override).
        // Takes effect on the next compile; we also re-resolve + regenerate the
        // IntelliSense config immediately so the editor updates.
        auto key  = xp::get_string_field(parsed->args_json, "key");
        auto path = xp::get_string_field(parsed->args_json, "path");
        if (!key) { send_rsp_err(srv, id, "missing key"); return; }
        // map UI key -> project.json field name
        std::string field;
        if      (*key == "include")   field = "include_dir";
        else if (*key == "opencv")    field = "opencv_dir";
        else if (*key == "turbojpeg") field = "turbojpeg_root";
        else if (*key == "ipp")       field = "ipp_root";
        else if (*key == "vcvars")    field = "vcvars";
        else { send_rsp_err(srv, id, "unknown toolchain key: " + *key); return; }
        std::string err;
        if (!xi::toolchain::write_override(g_eng.project_folder, field, path ? *path : std::string(), err)) {
            send_rsp_err(srv, id, "set_toolchain_override failed: " + err);
            return;
        }
        // Re-resolve globals so the next compile reflects the change; the editor's
        // IntelliSense config is the VS Code extension's job (it re-reads the
        // health below). The core no longer writes .vscode.
        resolve_toolchain_(g_eng.project_folder);
        std::string data = "{\"applied\":true,\"recompile_needed\":true,\"health\":";
        data += xi::toolchain::health_json(g_eng.project_folder, g_eng.include_dir_default);
        data += "}";
        send_rsp_ok(srv, id, data);
}
