//
// service_cmd_project_lc.cpp — PROJECT + working-copy lifecycle command handlers:
// open/close/create/save/load_project and commit/discard_working_copy, plus the
// small session handlers cmd:ping / cmd:version / cmd:shutdown (generic service
// lifecycle, homed here rather than with the narrow script-compile machinery).
// Split from the former service_cmd_lifecycle.cpp (behavior-preserving code motion).
//
// Deliberately NOT here: cmd:compile_and_load and the script hot-swap / cross-frame
// state migration live in service_cmd_script.cpp. The set_param / set_instance_def /
// list_params / list_instances handlers live in service_cmd_project.cpp. The
// guarded_* / script_grow_retry fault-boundary templates live in service_guard.hpp.
//
#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <yyjson.h>
#include <xi/xi_project.hpp>
#include <xi/xi_owner_lock.hpp>

#include "service_cmds.hpp"
#include "service_guard.hpp"          // script_grow_retry (save_project) + seh_exception (load set_def)
#include <xi/xi_health.hpp>            // IWYU: xi::health()/CompHealth/SysState (formerly transitive via service_state.hpp)
#include <xi/xi_plugin_manager.hpp>  // IWYU: PluginManager / InstanceRegistry (formerly transitive via service_state.hpp; now pimpl-hidden)
#include <xi/xi_script_loader.hpp>    // IWYU: LoadedScript members — list_params/set_param (formerly transitive; now pimpl-hidden)
#include <xi/xi_ws_server.hpp>

// ---- service session lifecycle: ping / version / shutdown ------------------
void cmd_ping_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), R"({"pong":true,"ts":%.3f})", now_seconds());
        send_rsp_ok(srv, id, buf);
}

void cmd_version_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        std::string vd = std::string(R"({"version":")") + XINSP2_VERSION
                       + R"(","commit":")" + XINSP2_COMMIT
                       + R"(","abi":2})";
        send_rsp_ok(srv, id, vd);
}

void cmd_shutdown_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Controlled teardown while everything is still alive, so nothing runs a
        // bus emit / module_lifetime deleter against a half-destroyed process at
        // static-destruction time. Single source of truth (see the function).
        controlled_shutdown_teardown_();
        send_rsp_ok(srv, id);
        g_eng.should_exit = true;
}

// ---- project + working-copy lifecycle --------------------------------------
void cmd_save_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        auto path = xp::get_string_field(parsed->args_json, "path");
        if (!path) { send_rsp_err(srv, id, "missing path"); return; }
        std::string params_json, inst_json;
        {
            std::lock_guard<std::mutex> lk(g_eng.script_mu);
            if (g_eng.script().ok()) {
                std::vector<char> buf(64 * 1024);
                // Wave-2 #4: these two copies LACKED the grow-retry half of the
                // buffer protocol entirely — a params/instances list over 64 KiB
                // returned -needed, the result was silently dropped, and
                // save_project wrote a project.json WITHOUT them (silent data
                // loss on save). The shared helper supplies the retry. No -1
                // error return here.
                if (g_eng.script().list_params) {
                    int n = script_grow_retry(buf, /*minus_one_is_terminal=*/false,
                        [&](char* b, int len) { return g_eng.script().list_params(b, len); });
                    if (n > 0) params_json.assign(buf.data(), (size_t)n);
                }
                if (g_eng.script().list_instances) {
                    int n = script_grow_retry(buf, /*minus_one_is_terminal=*/false,
                        [&](char* b, int len) { return g_eng.script().list_instances(b, len); });
                    if (n > 0) inst_json.assign(buf.data(), (size_t)n);
                }
            }
        }
        // Read–modify–write: load the existing document (if any) and overwrite
        // ONLY the keys this command owns (params/instances) + stamp schema,
        // preserving every other top-level key verbatim. The prior code rebuilt
        // project.json from just these two keys and silently dropped the rest
        // (runtime / parallelism / groups / plugin_dirs / plugins, and the
        // extension-owned params / auto_respawn / watchdog_ms) — data loss on a
        // normal save. write_text is atomic (temp + rename).
        std::string existing = xi::project::read_text(*path);
        std::string content = xi::project::merge_project_json(existing, params_json, inst_json);
        if (xi::project::write_text(*path, content)) {
            send_rsp_ok(srv, id);
        } else {
            send_rsp_err(srv, id, "failed to write " + *path);
        }
}

void cmd_commit_working_copy_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Mirror the <project>/.xinsp_work scratch back onto the canonical
        // project — the UI "Save Project" action. Persist any live instance
        // configs to the scratch first so the commit captures them.
        // Drain the dispatch pool first (same constraint as discard_working_copy
        // / open_project): the commit reads instance configs + does a filesystem
        // mirror (add/overwrite/delete) on the scratch that continuous workers
        // are concurrently reading/writing.
        auto _wc_commit_guard = quiesce_dispatch_for_lifecycle_op_("commit_working_copy", &srv);  // resumes at block end
        std::string save_fail;
        for (auto& [iname, _] : g_eng.plugin_mgr().project().instances) {
            if (!g_eng.plugin_mgr().save_instance(iname)) save_fail = iname;
        }
        if (!save_fail.empty()) {
            // An instance config couldn't reach disk (disk full / read-only) — the
            // scratch we're about to commit is itself stale, so don't claim success.
            send_rsp_err(srv, id, "failed to persist instance '" + save_fail +
                         "' before commit (disk full / read-only?)");
        } else if (g_eng.plugin_mgr().commit_working_copy(_wc_commit_guard.token())) {
            send_rsp_ok(srv, id, "{\"committed\":true,\"canonical\":" +
                        ([]{ std::string s; xp::json_escape_into(s, g_eng.plugin_mgr().canonical_path()); return s; }()) + "}");
        } else {
            send_rsp_err(srv, id, "no working copy active (open with working_copy:true)");
        }
}

void cmd_discard_working_copy_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Blow away the scratch + re-seed from canonical, then reopen. Same
        // teardown constraint as open_project — drain the dispatch pool first.
        if (!g_eng.plugin_mgr().has_working_copy()) {
            send_rsp_err(srv, id, "no working copy active");
            return;
        }
        auto _wc_discard_guard = quiesce_dispatch_for_lifecycle_op_("discard_working_copy", &srv);  // resumes at block end
        if (g_eng.plugin_mgr().reopen_fresh_working_copy(_wc_discard_guard.token())) {
            send_rsp_ok(srv, id, g_eng.plugin_mgr().to_json());
        } else {
            send_rsp_err(srv, id, "discard failed");
        }
}

void cmd_load_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // BEST-EFFORT / NON-ATOMIC import (review 04 #2/#3, P0-honesty). This handler
        // applies the recipe's params then its instance defs SEQUENTIALLY and does NOT
        // quiesce dispatch around the whole operation — it is not a transaction and does
        // not roll back a partial application. A param/instance that fails to apply is
        // collected as a warning; the recipe is left half-restored. So the RESULT must be
        // honest about that: an explicit top-level `status` distinguishes a clean load
        // ("ok") from a half-applied one ("partial") from a hard failure ("rejected"),
        // and `ok:false` is set on partial/rejected so generic clients (`if (resp.ok)
        // show "loaded"`) don't read a partial application as success. Making this
        // actually atomic is the deferred atomic-recipe rework — NOT done here.
        // A hard failure BEFORE any param/instance is touched: nothing was applied,
        // so status is "rejected" (and ok:false). Carry the status field so a generic
        // client sees the same shape it does on partial.
        auto send_rejected = [&](const std::string& err) {
            send_rsp_err(srv, id, err, "{\"status\":\"rejected\"}");
        };
        auto path = xp::get_string_field(parsed->args_json, "path");
        if (!path) { send_rejected("missing path"); return; }
        std::string content = xi::project::read_text(*path);
        if (content.empty()) { send_rejected("failed to read " + *path); return; }

        // Use yyjson to parse the project file properly
        yyjson_doc* doc = yyjson_read(content.c_str(), content.size(), 0);
        yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (!root) { send_rejected("invalid JSON in project file"); return; }

        // Restore params. Collect any that DON'T apply (unknown name / rejected
        // value) so a partially-restored recipe isn't reported as a clean success —
        // otherwise the operator thinks the full recipe loaded while some params
        // silently kept their old/default values (a fail-reads-as-pass for recipes).
        std::vector<std::string> param_warnings;
        yyjson_val* params = yyjson_obj_get(root, "params");
        if (params && yyjson_is_arr(params)) {
            size_t _i, _n; yyjson_val* item;
            yyjson_arr_foreach(params, _i, _n, item) {
                yyjson_val* nm = yyjson_obj_get(item, "name");
                yyjson_val* val = yyjson_obj_get(item, "value");
                if (nm && yyjson_is_str(nm) && val) {
                    char vbuf[64] = {};
                    // Format EXACTLY: an int with %lld (not "%g", which turns a big int
                    // like 1000000 into "1e+06" -> set_param's stoll yields 1) and a
                    // real with full precision (%.17g, like VAR's round-trip).
                    if (yyjson_is_int(val))       std::snprintf(vbuf, sizeof(vbuf), "%lld", (long long)yyjson_get_sint(val));
                    else if (yyjson_is_real(val)) std::snprintf(vbuf, sizeof(vbuf), "%.17g", yyjson_get_real(val));
                    else if (yyjson_is_bool(val)) std::snprintf(vbuf, sizeof(vbuf), "%s", yyjson_get_bool(val) ? "true" : "false");
                    else continue;
                    // Params live in the script DLL. Also write g_eng.param_cache so a
                    // later compile_and_load replays THESE loaded values, not a stale
                    // pre-load cache — without this, editing the script + recompiling
                    // silently reverted every param to whatever was last set_param'd
                    // before load_project (the replay shadow had never been refreshed).
                    std::lock_guard<std::mutex> lk(g_eng.script_mu);
                    if (g_eng.script().ok() && g_eng.script().set_param) {
                        int rc = g_eng.script().set_param(yyjson_get_str(nm), vbuf);
                        if (rc == 0) g_eng.param_cache[yyjson_get_str(nm)] = vbuf;
                        else param_warnings.push_back(std::string(yyjson_get_str(nm)) +
                                 (rc == -1 ? ": no such param" : ": value rejected"));
                    } else {
                        param_warnings.push_back(std::string(yyjson_get_str(nm)) + ": no script loaded");
                    }
                }
            }
        }

        // Restore instance configs. Like the params above, collect any that DON'T
        // apply so a partial restore isn't reported as a clean ok. Critically, the
        // instances saved by list_instances include SCRIPT-declared xi::Instance
        // objects, which live in the script DLL's OWN registry — NOT the backend
        // InstanceRegistry singleton. find() can't see them, so resolving through
        // the backend registry alone would silently drop every script-instance def
        // (the recipe loads green while the line runs on default thresholds/models:
        // a fail-reads-as-pass). So mirror the set_instance_def handler: try the
        // backend registry first, then fall through to g_eng.script().set_instance_def.
        std::vector<std::string> instance_warnings;
        yyjson_val* instances = yyjson_obj_get(root, "instances");
        if (instances && yyjson_is_arr(instances)) {
            size_t _i, _n; yyjson_val* item;
            yyjson_arr_foreach(instances, _i, _n, item) {
                yyjson_val* nm = yyjson_obj_get(item, "name");
                yyjson_val* def = yyjson_obj_get(item, "def");
                if (nm && yyjson_is_str(nm) && def) {
                    const char* iname = yyjson_get_str(nm);
                    char* def_str = yyjson_val_write(def, 0, NULL);
                    auto inst = xi::InstanceRegistry::instance().find(iname);
                    // set_def / set_instance_def enter plugin code — a throwing/faulting
                    // plugin must degrade to a recipe warning, not terminate the backend.
                    try {
                    if (inst) {
                        if (!inst->set_def(def_str))
                            instance_warnings.push_back(std::string(iname) + ": set_def returned false");
                    } else {
                        // Not a backend plugin-manager instance — try the script DLL's
                        // own registry (where script-declared instances live).
                        std::lock_guard<std::mutex> lk(g_eng.script_mu);
                        if (g_eng.script().ok() && g_eng.script().set_instance_def) {
                            int rc = g_eng.script().set_instance_def(iname, def_str);
                            // Mirror the param path above: write g_eng.instance_def_cache so a
                            // later compile_and_load replays THIS recipe's def, not a stale
                            // pre-load value — else editing the script + recompiling reverts
                            // the just-loaded instance to the source default.
                            if (rc == 0) g_eng.instance_def_cache[iname] = def_str;
                            else
                                instance_warnings.push_back(std::string(iname) + ": set_instance_def failed");
                        } else {
                            instance_warnings.push_back(std::string(iname) + ": instance not found");
                        }
                    }
                    } catch (const seh_exception& e) {
                        char msg[128];
                        std::snprintf(msg, sizeof(msg), ": set_def crashed 0x%08X", e.code);
                        instance_warnings.push_back(std::string(iname) + msg);
                        xi::recover_seh_stack_or_die(e.code, "load set_def");
                    } catch (const std::exception& e) {
                        instance_warnings.push_back(std::string(iname) + ": set_def threw: " + e.what());
                    }
                    free(def_str);
                }
            }
        }

        yyjson_doc_free(doc);
        // Honest result (review 04 #2/#3, P0). status distinguishes:
        //   "ok"      — every param + instance applied (warnings empty)  -> ok:true
        //   "partial" — some applied, some failed (warnings non-empty)   -> ok:false
        // A "rejected" (nothing applied) can only come from the pre-parse guards above
        // via send_rejected(); by the time we get here at least the parse succeeded, so
        // the two live outcomes are ok / partial. On partial we set ok:false so a generic
        // client (`if (resp.ok) show "loaded"`) does NOT read a half-applied recipe as a
        // clean load; the param_warnings/instance_warnings arrays stay (additive) so
        // detailed clients can still enumerate exactly what didn't apply.
        bool partial = !param_warnings.empty() || !instance_warnings.empty();
        std::string data = "{\"status\":";
        data += partial ? "\"partial\"" : "\"ok\"";
        data += ",\"param_warnings\":[";
        for (size_t i = 0; i < param_warnings.size(); ++i) {
            if (i) data += ",";
            xp::json_escape_into(data, param_warnings[i]);
        }
        data += "],\"instance_warnings\":[";
        for (size_t i = 0; i < instance_warnings.size(); ++i) {
            if (i) data += ",";
            xp::json_escape_into(data, instance_warnings[i]);
        }
        data += "]}";
        if (partial) {
            send_rsp_err(srv, id,
                "project loaded only partially — see param_warnings/instance_warnings",
                data);
        } else {
            send_rsp_ok(srv, id, data);
        }
}

void cmd_create_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        auto folder = xp::get_string_field(parsed->args_json, "folder");
        auto pname  = xp::get_string_field(parsed->args_json, "name");
        if (!folder || !pname) { send_rsp_err(srv, id, "missing folder or name"); return; }
        // Quiesce like open/close_project: create_project clears the previous
        // project's instances + registry entries (adapter dtors run) — before
        // this guard, a dispatch worker mid-inspect into one of those adapters
        // raced the teardown (the quiesce-hole kin of P0-AB-3). skip_resume:
        // whatever stream was running belonged to the project being replaced.
        auto create_guard = quiesce_dispatch_for_lifecycle_op_("create_project", &srv);
        // Wave-2 #3: create_project REPLACES the project just like open/close,
        // but never cleared the project-boundary state — the old project's bus
        // per-source map, param_cache / instance_def_cache replay shadows and
        // captured xi::kv() survived into the fresh project, so its first
        // compile_and_load replayed the REPLACED project's tuned values /
        // carried state (the documented cross-project leak class). Same reset,
        // same point in the sequence as open_project (before the PM swap).
        reset_project_boundary_state_();
        const bool created = g_eng.plugin_mgr().create_project(create_guard.token(), *folder, *pname);
        create_guard.skip_resume();   // fresh empty project — nothing to stream; the launch pause is still released by the guard's destructor
        if (created) {
            send_rsp_ok(srv, id, g_eng.plugin_mgr().to_json());
        } else {
            send_rsp_err(srv, id, "failed to create project");
        }
}

void cmd_open_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Accept either `folder` (historical) or `path` (matches what
        // the protocol doc + Python SDK / load_project use). Same arg,
        // different name; this defuses the inconsistency the AI agent
        // hit on the size-buckets case.
        auto folder = xp::get_string_field(parsed->args_json, "folder");
        if (!folder) folder = xp::get_string_field(parsed->args_json, "path");
        if (!folder) { send_rsp_err(srv, id, "missing folder/path"); return; }
        // P0-AB-3: must drain dispatch pool BEFORE the old project is
        // torn down. open_project (the PluginManager method) destroys
        // the previous project's instances and FreeLibrary's its
        // plugin DLLs; if a worker is mid-inspect into a now-freed
        // plugin function we SEGV.
        // working_copy: operate on a <project>/.xinsp_work scratch copy
        // (resume if present, else seed) so edits are transactional + crash-
        // durable. Default false = legacy in-place behaviour.
        bool working_copy = xp::get_bool_field(parsed->args_json, "working_copy");
        // O2 (round-10 red-team): the launch-pause must be HELD across open_project()'s
        // teardown of the OLD project (it FreeLibrary's the old plugin DLLs) — releasing
        // it early let a straggler source-emit one-shot launch (paused_==0) into an
        // unmapped old DLL (use-after-unload). skip_resume() below only skips the
        // continuous-resume (the new project drives its own autostart); the pause is
        // released by the guard's destructor at scope end — an early release is no
        // longer expressible.
        auto open_guard = quiesce_dispatch_for_lifecycle_op_("open_project", &srv);
        // Wave-2 #3: drop everything owned by the project being replaced — the
        // stale bus sink + per-source emit-time map AND the script replay
        // shadows / kv channel (see reset_project_boundary_state_ for the full
        // cross-project-leak rationale) — before tearing it down + opening the
        // new one. A fresh project starts from its own file-scope defaults.
        reset_project_boundary_state_();
        const bool opened = g_eng.plugin_mgr().open_project(open_guard.token(), *folder, working_copy);
        open_guard.skip_resume();   // skip the continuous-resume at scope end (new project autostarts); the launch pause is released by the guard's DESTRUCTOR, never early (O2)
        if (opened) {
            // F5: advisory single-writer stamp. If another LIVE backend already
            // owns this canonical, warn — two writers to one project clobber each
            // other when a working-copy commit mirrors over the canonical. A stale
            // stamp (the owning pid is gone) is silently taken over; never refuses.
            {
                auto prev = xi::ownerlock::read(*folder);
                if (prev.present && prev.pid != xi::ownerlock::self_pid() &&
                    xi::ownerlock::pid_alive(prev.pid)) {
                    std::string s = "project may already be open in another backend (pid "
                        + std::to_string(prev.pid) + "); concurrent writes can be lost "
                        "when a working-copy commit mirrors over them";
                    xp::LogMsg lm; lm.level = "warn"; lm.msg = s; srv.send_text(lm.to_json());
                    push_recent_error("open_project", s);
                }
                xi::ownerlock::write(*folder, xi::wall_ms());
            }
            auto& proj = g_eng.plugin_mgr().project();
            int inst_count = (int)proj.instances.size();
            std::fprintf(stderr, "[xinsp2] project opened: %s (%d instances)\n",
                         proj.name.c_str(), inst_count);
            // Apply project.json "runtime" knobs. process_priority is live now;
            // timer_fps seeds the live timer rate (0 = trigger-only) for when
            // continuous mode runs.
            apply_process_priority_(proj.runtime_priority);
            if (proj.runtime_timer_fps >= 0)
                g_eng.timer_interval_ms.store(proj.runtime_timer_fps > 0 ? std::max(1, 1000 / proj.runtime_timer_fps) : 0);
            for (auto& [k, v] : proj.instances) {
                std::fprintf(stderr, "[xinsp2]   instance: %s (%s)\n",
                             k.c_str(), v.plugin_name.c_str());
            }
            // Surface skip-bad-instance warnings to the user. The project
            // open still succeeds; bad instances are simply absent from
            // the runtime registry. Extension can show a toast.
            auto warns = g_eng.plugin_mgr().open_warnings();
            if (!warns.empty()) {
                std::string s = "project opened with " + std::to_string(warns.size())
                              + " skipped instance(s):";
                for (auto& w : warns) {
                    s += "\n  - " + w.instance;
                    if (!w.plugin.empty()) s += " (" + w.plugin + ")";
                    s += ": " + w.reason;
                }
                xp::LogMsg lm;
                lm.level = "warn";
                lm.msg = s;
                srv.send_text(lm.to_json());
            }
            // Remember the canonical folder + apply any per-project toolchain
            // override (project.json "toolchain" block) so the compiler and the
            // IntelliSense config below both pick up the user's path fixes.
            g_eng.project_folder = *folder;
            // Health contract: a project is open (boot/prior → project_loaded). Any
            // prior project's runtime-fault overlay belongs to the project we just
            // replaced, so drop it. The new project's instances register their
            // health lazily (derived from InstState at get_health time).
            xi::health().clear_all_instance_degraded();
            xi::health().set_state(xi::SysState::ProjectLoaded);
            resolve_toolchain_(*folder);
            // Put the project folder on the DLL search path so a script's
            // statically-linked external dep DLL can live in the project folder.
            set_project_dll_search_(*folder);
            // (IDE IntelliSense config: the VS Code extension writes
            // <project>/.vscode/c_cpp_properties.json itself, reading the compile
            // paths via cmd:toolchain_health — the core no longer touches .vscode.)
            send_rsp_ok(srv, id, g_eng.plugin_mgr().to_json());
        } else {
            // Prefer a specific hard-refusal reason (e.g. an unrecognized future
            // project-file schema) over the generic message.
            std::string oe = g_eng.plugin_mgr().open_error();
            send_rsp_err(srv, id, !oe.empty() ? oe : ("failed to open project in " + *folder));
        }
}

void cmd_close_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // P0-AB-3: must drain dispatch pool BEFORE close_project tears
        // down instances and FreeLibrary's plugin DLLs. (PR #33 fixed
        // the in-PluginManager teardown order; this fixes the
        // dispatcher-still-running case the dispatcher pool hit when
        // close_project is sent during continuous mode.)
        // Health contract: if dispatch was live, the quiesce below drains it →
        // mark `draining` before the pool teardown, `boot` after.
        {
            xi::SysState s = xi::health().state();
            if (s == xi::SysState::Running || s == xi::SysState::Degraded)
                xi::health().set_state(xi::SysState::Draining);
        }
        // O2 (round-10 red-team): the detached-launch pause stays HELD across the teardown
        // below. close_project() FreeLibrary's the plugin DLLs; a source-emit thread that
        // already snapshotted the bus sink (before the quiesce's clear_sink) could otherwise
        // launch a detached one-shot — inflight.launch would see paused_==0 — that runs
        // concurrently with the unload and calls into an unmapped DLL (use-after-unload).
        // skip_resume() after close_project() only skips the continuous-resume (no project
        // to stream); the pause itself is released by the guard's destructor at scope end —
        // the old dismiss()'s early unpause is no longer expressible.
        auto close_guard = quiesce_dispatch_for_lifecycle_op_("close_project", &srv);
        // Wave-2 #3: drop the bus's captured sink (it points at `srv`) BEFORE
        // the plugin DLLs are unloaded — otherwise the stale sink can fire into
        // a torn-down project — and clear the script replay shadows + kv, which
        // belong to the project being closed (see reset_project_boundary_state_
        // for the cross-project-leak rationale).
        reset_project_boundary_state_();
        g_eng.plugin_mgr().close_project(close_guard.token());
        close_guard.skip_resume();   // skip the continuous-resume at scope end (no project to stream); the launch pause is released by the guard's DESTRUCTOR, never early (O2)
        clear_inst_state();   // instances are gone — drop host-tracked state
        // Health contract: no project → `boot`. The instances (and their runtime-
        // fault overlay) are gone; the script survives a close (its DLL is not
        // unloaded here), so its health component is left intact.
        xi::health().clear_all_instance_degraded();
        xi::health().set_state(xi::SysState::Boot);
        // (script replay shadows + kv already cleared by
        // reset_project_boundary_state_ above — one primitive, no second copy.)
        send_rsp_ok(srv, id, "{\"closed\":true}");
}

