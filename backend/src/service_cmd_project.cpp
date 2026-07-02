//
// service_cmd_project.cpp — project-CRUD command handlers, split from
// service_main.cpp (behavior-preserving; see service_internal.hpp).
//
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <yyjson.h>
#include <xi/xi_project.hpp>

#include "service_internal.hpp"

// ---- project-CRUD ----------------------------------------------------------
void cmd_list_params_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // If a script is loaded, delegate to its own registry thunk so we
        // see the DLL's params. Otherwise report the backend's own.
        std::string params_json;
        {
            std::lock_guard<std::mutex> lk(g_eng.script_mu);
            if (g_eng.script.ok() && g_eng.script.list_params) {
                std::vector<char> buf(64 * 1024);
                int n = g_eng.script.list_params(buf.data(), (int)buf.size());
                if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024);  // widen: -INT_MIN is UB
                             n = g_eng.script.list_params(buf.data(), (int)buf.size()); }
                if (n > 0) params_json.assign(buf.data(), (size_t)n);
            }
        }
        if (params_json.empty()) params_json = "[]";   // params live in the script DLL
        std::string out = "{\"type\":\"instances\",\"instances\":[],\"params\":";
        out += params_json;
        out += "}";
        send_rsp_ok(srv, id, "{}");
        srv.send_text(out);
}

void cmd_set_param_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        auto pname = xp::get_string_field(parsed->args_json, "name");
        if (!pname) {
            send_rsp_err(srv, id, "set_param: missing name");
            return;
        }
        // Extract the value's RAW JSON token and pass it verbatim to set_from_json.
        // The old path reformatted the number via "%g", which turned a big integer
        // (e.g. area_min=1000000) into "1e+06" -> std::stoll stops at 'e' -> the param
        // was SILENTLY set to 1; floats lost precision too (%g = 6 sig figs). It also
        // scanned the whole args for "value":true, which could false-match a string
        // value. find_key returns only the top-level value token (a number / true|false
        // / "quoted string"), exact and un-reformatted; the param's set_from_json
        // validates it (rc -2 if it rejects). A missing value isn't a missing param.
        std::string val;
        const char* after = nullptr;
        if (!xp::detail::find_key(parsed->args_json.data(),
                                  parsed->args_json.data() + parsed->args_json.size(),
                                  "value", val, after)) {
            send_rsp_err(srv, id, "set_param: missing 'value' for '" + *pname + "'");
            return;
        }
        // xi_script_set_param contract: 0 = set, -1 = no such param, -2 = the param
        // exists but rejected this value (set_from_json failed).
        int rc = 0; bool called = false;
        {
            std::lock_guard<std::mutex> lk(g_eng.script_mu);
            if (g_eng.script.ok() && g_eng.script.set_param) {
                called = true;
                rc = g_eng.script.set_param(pname->c_str(), val.c_str());
                // Cache an accepted value so compile_and_load replays it into the next
                // DLL load (else the new DLL's file-scope default silently overwrites it).
                if (rc == 0) g_eng.param_cache[*pname] = val;
            }
        }
        if (!called) { send_rsp_err(srv, id, "set_param: no script loaded"); return; }
        if (rc == 0) { send_rsp_ok(srv, id); return; }
        if (rc == -1) { send_rsp_err(srv, id, std::string("no such param: ") + *pname); return; }
        send_rsp_err(srv, id, "set_param: '" + *pname + "' rejected the value (out of range / wrong type)");
}

void cmd_list_instances_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        std::string inst_json, params_json;
        {
            std::lock_guard<std::mutex> lk(g_eng.script_mu);
            if (g_eng.script.ok()) {
                std::vector<char> buf(64 * 1024);
                if (g_eng.script.list_instances) {
                    int n = g_eng.script.list_instances(buf.data(), (int)buf.size());
                    if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024); n = g_eng.script.list_instances(buf.data(), (int)buf.size()); }
                    if (n > 0) inst_json.assign(buf.data(), (size_t)n);
                }
                if (g_eng.script.list_params) {
                    int n = g_eng.script.list_params(buf.data(), (int)buf.size());
                    if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024); n = g_eng.script.list_params(buf.data(), (int)buf.size()); }
                    if (n > 0) params_json.assign(buf.data(), (size_t)n);
                }
            }
        }
        // Also include backend-managed instances (from PluginManager)
        auto& proj = g_eng.plugin_mgr.project();
        std::string backend_inst = "[";
        int bi = 0;
        for (auto& [k, v] : proj.instances) {
            if (bi++) backend_inst += ",";
            backend_inst += "{\"name\":\"" + v.name + "\",\"plugin\":\"" + v.plugin_name + "\"}";
        }
        backend_inst += "]";

        // Merge: script instances + backend instances
        std::string merged_inst;
        if (!inst_json.empty() && inst_json != "[]" && bi > 0) {
            // Both have entries — merge arrays
            merged_inst = inst_json.substr(0, inst_json.size() - 1) + "," + backend_inst.substr(1);
        } else if (bi > 0) {
            merged_inst = backend_inst;
        } else {
            merged_inst = inst_json.empty() ? "[]" : inst_json;
        }

        std::string out = "{\"type\":\"instances\",\"instances\":";
        out += merged_inst;
        out += ",\"params\":";
        out += params_json.empty() ? "[]" : params_json;
        out += "}";
        send_rsp_ok(srv, id, "{}");
        srv.send_text(out);
}

void cmd_set_instance_def_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        // Extract the def object as a raw JSON substring
        std::string def_str;
        const char* after;
        if (xp::detail::find_key(parsed->args_json.data(),
                                  parsed->args_json.data() + parsed->args_json.size(),
                                  "def", def_str, after)) {
            // def_str is the raw JSON value
        } else {
            def_str = "{}";
        }
        // Try backend's InstanceRegistry first (plugin-manager instances)
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        if (inst) {
            // set_def enters plugin code (C-ABI) — guard like exchange_instance so a
            // throwing/faulting plugin returns a clean error instead of terminating.
            try {
                if (inst->set_def(def_str)) {
                    set_inst_state(*iname, InstState::Active);
                    send_rsp_ok(srv, id);
                } else {
                    set_inst_state(*iname, InstState::Faulted, "set_def returned false");
                    send_rsp_err(srv, id, "set_def returned false");
                }
            } catch (const seh_exception& e) {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "set_def '%s' crashed: 0x%08X (%s)",
                             iname->c_str(), e.code, e.what());
                set_inst_state(*iname, InstState::Faulted, msg);
                send_rsp_err(srv, id, msg);
            } catch (const std::exception& e) {
                std::string em = std::string("set_def error: ") + e.what();
                set_inst_state(*iname, InstState::Faulted, em);
                send_rsp_err(srv, id, em);
            }
        } else {
            std::lock_guard<std::mutex> lk(g_eng.script_mu);
            if (g_eng.script.ok() && g_eng.script.set_instance_def) {
                try {
                    int rc = g_eng.script.set_instance_def(iname->c_str(), def_str.c_str());
                    // Cache an accepted def so compile_and_load replays it into the next
                    // DLL load (else the new DLL's file-scope ctor silently reverts it).
                    if (rc == 0) g_eng.instance_def_cache[*iname] = def_str;
                    if (rc == 0) { set_inst_state(*iname, InstState::Active); send_rsp_ok(srv, id); }
                    else { set_inst_state(*iname, InstState::Faulted, "set_instance_def failed");
                           send_rsp_err(srv, id, "set_instance_def failed"); }
                } catch (const seh_exception& e) {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg), "script set_instance_def '%s' crashed: 0x%08X (%s)",
                                 iname->c_str(), e.code, e.what());
                    set_inst_state(*iname, InstState::Faulted, msg);
                    send_rsp_err(srv, id, msg);
                } catch (const std::exception& e) {
                    std::string em = std::string("script set_instance_def error: ") + e.what();
                    set_inst_state(*iname, InstState::Faulted, em);
                    send_rsp_err(srv, id, em);
                }
            } else {
                send_rsp_err(srv, id, "instance not found: " + *iname);
            }
        }
}

void cmd_get_instance_def_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Symmetric read of set_instance_def: returns the instance's full def
        // JSON (incl. any assets the plugin round-trips, e.g. image_png_b64), so
        // a host can snapshot an instance without scraping exchange:get_status.
        // Loop over list_instances to snapshot a whole project (the foundation
        // for portable product/instrument config bundles).
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        // Backend's InstanceRegistry first (plugin-manager instances).
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        if (inst) {
            // get_def enters plugin code (C-ABI) — guard like exchange_instance.
            try {
                std::string def = inst->get_def();
                send_rsp_ok(srv, id, def.empty() ? "{}" : def);
            } catch (const seh_exception& e) {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "get_def '%s' crashed: 0x%08X (%s)",
                             iname->c_str(), e.code, e.what());
                send_rsp_err(srv, id, msg);
            } catch (const std::exception& e) {
                send_rsp_err(srv, id, std::string("get_def error: ") + e.what());
            }
        } else {
            std::lock_guard<std::mutex> lk(g_eng.script_mu);
            if (g_eng.script.ok() && g_eng.script.get_instance_def) {
                try {
                    std::vector<char> buf(256 * 1024);
                    int n = g_eng.script.get_instance_def(iname->c_str(), buf.data(), (int)buf.size());
                    if (n < 0 && n != -1) {   // -needed → grow + retry (-1 = not found)
                        buf.resize((size_t)(-(int64_t)n) + 1024);
                        n = g_eng.script.get_instance_def(iname->c_str(), buf.data(), (int)buf.size());
                    }
                    if (n >= 0) send_rsp_ok(srv, id, std::string(buf.data(), (size_t)n));
                    else        send_rsp_err(srv, id, "instance not found: " + *iname);
                } catch (const seh_exception& e) {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg), "script get_instance_def '%s' crashed: 0x%08X (%s)",
                                 iname->c_str(), e.code, e.what());
                    send_rsp_err(srv, id, msg);
                } catch (const std::exception& e) {
                    send_rsp_err(srv, id, std::string("script get_instance_def error: ") + e.what());
                }
            } else {
                send_rsp_err(srv, id, "instance not found: " + *iname);
            }
        }
}

void cmd_create_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        auto iname  = xp::get_string_field(parsed->args_json, "name");
        auto plugin = xp::get_string_field(parsed->args_json, "plugin");
        if (!iname || !plugin) { send_rsp_err(srv, id, "missing name or plugin"); return; }
        // Ensure plugin is loaded — surface WHY if it can't be (missing DLL,
        // missing factory symbol, ABI mismatch, etc.) instead of a generic failure.
        std::string load_err;
        if (!g_eng.plugin_mgr.load_plugin(*plugin, &load_err)) {
            send_rsp_err(srv, id, load_err.empty() ? "failed to load plugin" : load_err);
            return;
        }
        // G2.1 — create() runs the plugin's factory (untrusted native code); stamp
        // the culprit so a factory fault is attributed to this plugin.
        stamp_culprit_(iname->c_str(), *plugin);
        std::string create_err;
        auto* ii = g_eng.plugin_mgr.create_instance(*iname, *plugin, &create_err);
        if (ii) {
            // create_instance records the Created state internally (atomic with the
            // instance add) — no separate set_inst_state needed.
            send_rsp_ok(srv, id, g_eng.plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, create_err.empty() ? "failed to create instance" : create_err);
        }
}

void cmd_remove_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        bool delete_folder =
            parsed->args_json.find("\"delete_folder\":true") != std::string::npos;
        if (g_eng.plugin_mgr.remove_instance(*iname, delete_folder)) {
            // remove_instance drops the lifecycle state internally (atomic with the
            // unregister) — no separate drop_inst_state needed.
            send_rsp_ok(srv, id, g_eng.plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "instance not found: " + *iname);
        }
}

void cmd_rename_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        auto old_name = xp::get_string_field(parsed->args_json, "name");
        auto new_name = xp::get_string_field(parsed->args_json, "new_name");
        if (!old_name || !new_name) { send_rsp_err(srv, id, "missing name or new_name"); return; }
        using RR = xi::PluginManager::RenameResult;
        RR rr = g_eng.plugin_mgr.rename_instance(*old_name, *new_name);
        if (rr == RR::Rejected) {
            send_rsp_err(srv, id, "rename failed — name in use or instance missing");
        } else {
            // Ok OR NotPersisted: the runtime + folder were renamed. rename_instance
            // already migrated the host-tracked state inside the same locked op, so
            // there's nothing to sync here — only the disk-save status differs.
            if (rr == RR::NotPersisted)
                send_rsp_err(srv, id, "renamed in memory but could not persist to disk "
                                      "(disk full / read-only?) — may revert on restart");
            else
                send_rsp_ok(srv, id, g_eng.plugin_mgr.to_json());
        }
}

void cmd_get_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        send_rsp_ok(srv, id, g_eng.plugin_mgr.to_json());
}

void cmd_save_instance_config_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        if (g_eng.plugin_mgr.save_instance(*iname)) {
            send_rsp_ok(srv, id);
        } else {
            send_rsp_err(srv, id, "instance not found: " + *iname);
        }
}

void cmd_get_dashboard_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Serve the project's HMI dashboard so the HMI only ever needs the BE WS
        // URL (no filesystem coupling). Reads <project>/dashboard[.<name>].json.
        // args: { name?: string }. data: { found, name, dashboard:<verbatim JSON> }.
        auto nm = xp::get_string_field(parsed->args_json, "name");
        std::string fname = (nm && !nm->empty()) ? ("dashboard." + *nm + ".json") : "dashboard.json";
        // Guard the name against path escapes (only a simple token allowed).
        bool bad = fname.find("..") != std::string::npos || fname.find('/') != std::string::npos
                || fname.find('\\') != std::string::npos;
        std::string content;
        bool found = false;
        if (!bad && !g_eng.project_folder.empty()) {
            std::ifstream f(std::filesystem::path(g_eng.project_folder) / fname, std::ios::binary);
            if (f) { std::ostringstream ss; ss << f.rdbuf(); content = ss.str(); found = !content.empty(); }
        }
        std::string out = "{\"found\":" + std::string(found ? "true" : "false") + ",\"name\":";
        xp::json_escape_into(out, (nm && !nm->empty()) ? *nm : "");
        if (found) out += ",\"dashboard\":" + content;   // verbatim file (already JSON)
        out += "}";
        send_rsp_ok(srv, id, out);
}

