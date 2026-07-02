//
// service_cmd_lifecycle.cpp — lifecycle command handlers, split from
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
#include <xi/xi_owner_lock.hpp>
#include <xi/xi_script_compiler.hpp>

#include "service_internal.hpp"

// ---- lifecycle -------------------------------------------------------------
void cmd_ping_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), R"({"pong":true,"ts":%.3f})", now_seconds());
        send_rsp_ok(srv, id, buf);
}

void cmd_version_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        std::string vd = std::string(R"({"version":")") + XINSP2_VERSION
                       + R"(","commit":")" + XINSP2_COMMIT
                       + R"(","abi":1})";
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

void cmd_compile_and_load_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        auto src = xp::get_string_field(parsed->args_json, "path");
        if (!src) {
            send_rsp_err(srv, id, "compile_and_load: missing path");
            return;
        }

        // Stop continuous mode before reloading — the worker thread holds
        // function pointers into the DLL we're about to unload. Remember whether
        // the run was active so we can re-arm it after the reload — without this,
        // scripts that get hot-reloaded mid-run would silently halt and the caller
        // would have to know to re-issue cmd:start.
        bool was_continuous = false;
        int  prior_continuous_fps = 10;
        if (g_eng.continuous.load()) {
            was_continuous = true;
            prior_continuous_fps = g_eng.continuous_fps.load();
            stop_dispatch_pool_();
            std::fprintf(stderr, "[xinsp2] stopped continuous mode for reload (will resume)\n");
        }

        // Resume continuous exactly as it was before the reload. MUST be called on
        // every exit path — a bare `return` on a compile error (a typo in the
        // script, the common case) or a load failure would otherwise leave the
        // stream stopped and force the client to re-issue cmd:start to recover.
        auto resume_continuous_if_needed = [&]() {
            if (!was_continuous) return;
            bool trig_only = prior_continuous_fps <= 0;
            int fps = trig_only ? 0 : prior_continuous_fps;
            g_eng.continuous_fps = fps;
            g_eng.continuous = true;
            int interval_ms = trig_only ? 0 : std::max(1, 1000 / std::max(fps, 1));
            spawn_group_pool_(&srv, interval_ms);
            std::fprintf(stderr, "[xinsp2] continuous mode resumed\n");
        };

        // AOT / no-toolchain bundle: a `.dll` path is loaded DIRECTLY (no cl.exe).
        // Resolve relative to the project folder. Otherwise compile the .cpp.
        bool prebuilt = src->size() > 4 &&
            (src->compare(src->size() - 4, 4, ".dll") == 0 || src->compare(src->size() - 4, 4, ".DLL") == 0);
        xi::script::CompileResult res;
        if (prebuilt) {
            std::filesystem::path p(*src);
            if (p.is_relative() && !g_eng.project_folder.empty()) p = std::filesystem::path(g_eng.project_folder) / p;
            // Containment: a prebuilt DLL MUST resolve inside the open project
            // folder. Without this an absolute/UNC `.dll` path loads an
            // arbitrary DLL — its DllMain / static initializers execute
            // in-process — and the WS port is unauthenticated by default.
            // Legitimate AOT bundles ship the DLL inside the project and
            // reference it relatively (already resolved above), so this only
            // rejects out-of-tree paths.
            std::error_code ec1, ec2;
            auto canon_dll  = std::filesystem::weakly_canonical(p, ec1);
            auto canon_proj = g_eng.project_folder.empty()
                ? std::filesystem::path{}
                : std::filesystem::weakly_canonical(std::filesystem::path(g_eng.project_folder), ec2);
            bool contained = !canon_proj.empty() && !ec1 && !ec2;
            if (contained) {
                auto rel = canon_dll.lexically_relative(canon_proj);
                contained = !rel.empty() && *rel.begin() != "..";
            }
            if (!contained) {
                std::fprintf(stderr, "[xinsp2] refused out-of-tree prebuilt DLL: %s\n", p.string().c_str());
                send_rsp_err(srv, id,
                    "prebuilt DLL must be inside the project folder (out-of-tree path refused)");
                // P1-4: sticky degraded marker so a status poll sees it after the rsp.
                set_status_internal("@compile", "degraded: prebuilt DLL refused (out-of-tree)");
                // This return is past stop_dispatch_pool_() — like the compile/load
                // failure paths it must re-arm continuous mode or the stream stays
                // dead until the client re-issues cmd:start.
                resume_continuous_if_needed();
                return;
            }
            res.ok = true;
            res.dll_path = canon_dll.string();
            std::fprintf(stderr, "[xinsp2] AOT: loading prebuilt script DLL (no compile): %s\n", res.dll_path.c_str());
        } else {
        xi::script::CompileRequest req;
        req.source_path     = *src;
        req.output_dir      = (std::filesystem::path(g_eng.work_dir) / "script_build").string();
        req.include_dir     = g_eng.include_dir;
        req.opencv_dir      = g_eng.opencv_dir;
        req.turbojpeg_root  = g_eng.turbojpeg_root;
        req.ipp_root        = g_eng.ipp_root;
        req.vcvars_path     = g_eng.tc_vcvars;   // empty = compiler auto-finds vcvars64.bat
        // Project-declared external deps (project.json include_dirs / link_libs).
        read_script_deps_(g_eng.project_folder, req.include_dirs, req.link_libs,
                          req.openmp_max_threads, req.allow_raw_omp);
        // Fast dev compile (/Od) by default — the interactive edit→run loop wants
        // fast COMPILE, not fast runtime. A client benchmarking / the autostart
        // boot path passes "optimize":true to get /O2. (Both spacings, like has_ui.)
        bool want_optimize = parsed->args_json.find("\"optimize\":true") != std::string::npos ||
                             parsed->args_json.find("\"optimize\": true") != std::string::npos;
        req.fast = !want_optimize;

        // P2-6: emit a `compile_started` event before kicking off cl.exe.
        // compile_and_load is a request/response that can take 4+ s on a
        // fresh checkout; without this event drivers see a silent WS for
        // multiple seconds and can't show "compiling..." UI. data carries
        // the source path so concurrent compiles can be disambiguated.
        {
            xp::Event ev;
            ev.name = "compile_started";
            std::string data = "{\"path\":";
            xp::json_escape_into(data, *src);
            data += "}";
            ev.data_json = data;
            srv.send_text(ev.to_json());
        }

        res = xi::script::compile(req);

        // Pair the started event with a finished event so drivers can
        // bracket the operation. Carries `ok` and a short summary so a
        // listener that missed the rsp can still tell success from
        // failure.
        {
            xp::Event ev;
            ev.name = "compile_finished";
            std::string data = "{\"path\":";
            xp::json_escape_into(data, *src);
            data += ",\"ok\":";
            data += (res.ok ? "true" : "false");
            data += "}";
            ev.data_json = data;
            srv.send_text(ev.to_json());
        }
        }  // end else (compile path)

        // Serialize diagnostics for both error & success paths so the
        // extension can drive Problems panel / squiggles either way.
        auto build_diag_json = [&]() -> std::string {
            std::string s = "[";
            for (size_t i = 0; i < res.diagnostics.size(); ++i) {
                auto& d = res.diagnostics[i];
                if (i) s += ",";
                s += "{\"file\":";  xp::json_escape_into(s, d.file);
                s += ",\"line\":" + std::to_string(d.line);
                s += ",\"col\":"  + std::to_string(d.col);
                s += ",\"severity\":"; xp::json_escape_into(s, d.severity);
                s += ",\"code\":";    xp::json_escape_into(s, d.code);
                s += ",\"message\":"; xp::json_escape_into(s, d.message);
                s += "}";
            }
            s += "]";
            return s;
        };

        if (!res.ok) {
            std::string data = "{\"diagnostics\":" + build_diag_json() + "}";
            xp::Rsp r;
            r.id = id;
            r.ok = false;
            r.error = "compile failed";
            r.data_json = data;
            srv.send_text(r.to_json());
            xp::LogMsg lm;
            lm.level = "error";
            lm.msg = res.build_log;
            srv.send_text(lm.to_json());
            // P1-4: the rsp ok:false only reaches THIS caller; publish a sticky
            // "@compile" marker so a later status poll (or a reconnecting operator)
            // can tell the line is running the last-good def in a degraded state.
            set_status_internal("@compile", "degraded: compile failed");
            resume_continuous_if_needed();   // keep streaming the last-good script
            return;
        }

        {
            // Wait out any in-flight detached cmd:run before swapping the script
            // DLL (it holds g_eng.run_mu for the whole inspect and runs from the old
            // module). Order is g_eng.run_mu -> g_eng.script_mu, matching the run path.
            std::lock_guard<std::mutex> rl(g_eng.run_mu);
            std::lock_guard<std::mutex> lk(g_eng.script_mu);
            // Load the NEW DLL into a temporary first; only swap it in on
            // success. A failed load (bad DLL, missing export) then leaves the
            // previously-working script — and the client's subscriptions —
            // intact, instead of unloading the old one and wedging to
            // a null script. (temp-load-then-swap.)
            xi::script::LoadedScript next;
            std::string err;
            if (!xi::script::load_script(res.dll_path, next, err)) {
                send_rsp_err(srv, id, err);
                set_status_internal("@compile", "degraded: script load failed");  // P1-4
                resume_continuous_if_needed();   // old g_eng.script untouched, keep it streaming
                return;
            }
            // Save persistent state from the OLD DLL before swapping it out.
            // Stamp the OLD DLL's schema version alongside so restore into the
            // new DLL can detect a shape mismatch.
            if (g_eng.script.ok() && g_eng.script.get_state) {
                std::vector<char> buf(256 * 1024);
                int n = g_eng.script.get_state(buf.data(), (int)buf.size());
                if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024);
                             n = g_eng.script.get_state(buf.data(), (int)buf.size()); }
                if (n > 0) g_eng.persistent_state_json.assign(buf.data(), (size_t)n);
                g_eng.persistent_state_schema = g_eng.script.state_schema_version
                                          ? g_eng.script.state_schema_version()
                                          : 0;
            }
            // Swap: move-assign drops the old module's last ref — its deleter
            // does the owner-sweep + FreeLibrary once any in-flight inspect that
            // snapshotted it returns.
            g_eng.script = std::move(next);
            // The new DLL is now the active one. Bump the active-script
            // generation EXACTLY here — this is the only point a compiled DLL
            // becomes what `inspect` calls. A failed compile (returns above with
            // "compile failed") or a failed load (returns above) never reaches
            // this line, so the last-good script keeps its generation. relaxed:
            // publication happens-before via g_eng.script_mu, which every run
            // also holds when it snapshots the script + generation.
            g_eng.script_generation.fetch_add(1, std::memory_order_relaxed);
            // Output-sink subscriptions live entirely in the plugin (e.g. the
            // `expose` plugin tracks subscribed channels) — the core holds no
            // per-viewer subscription state across a recompile swap; binary frames
            // are a plain broadcast (emit_binary) and the plugin/its UI own routing.
            crash_set(crash_ctx().last_script, sizeof(crash_ctx().last_script),
                      res.dll_path.c_str());
            crash_set(crash_ctx().last_cmd, sizeof(crash_ctx().last_cmd),
                      "compile_and_load");
            // Wire xi::use() callbacks so the script can call back into backend.
            // host_api lets the script allocate/read images in the BACKEND pool —
            // plugins only see that pool via their own host_api, so script-side
            // ImagePool handles would be invisible to them.
            if (g_eng.script.set_use_callbacks) {
                g_eng.script.set_use_callbacks(
                    (void*)use_process_cb,
                    (void*)use_exchange_cb,
                    (void*)use_grab_cb,
                    (void*)script_host_api_());
            }
            // Phase 3: trigger access callbacks. Older scripts that don't
            // import xi_script_set_trigger_callbacks just stay null and
            // xi::current_trigger().is_active() returns false.
            if (g_eng.script.set_trigger_callbacks) {
                g_eng.script.set_trigger_callbacks(
                    (void*)trigger_info_cb,
                    (void*)trigger_image_cb,
                    (void*)trigger_sources_cb);
            }
            // Optional newer symbol — scripts compiled before P2-2
            // simply don't export it and t.primary_source() falls back
            // to sources().front().
            if (g_eng.script.set_trigger_leader_callback) {
                g_eng.script.set_trigger_leader_callback((void*)trigger_leader_cb);
            }
            // ABI v5: metadata-doc callback. Scripts compiled before this symbol
            // existed simply don't export it and current_trigger().meta() returns
            // an empty Record.
            if (g_eng.script.set_trigger_meta_callback) {
                g_eng.script.set_trigger_meta_callback((void*)trigger_meta_cb);
            }
            // Status callback. Scripts without xi_status.hpp leave this null
            // and xi::status() is a no-op.
            if (g_eng.script.set_status_callback) {
                g_eng.script.set_status_callback((void*)status_cb);
            }
            // C1: image-pool owner get/set thunks. Lets xi::async / xi::parallel_for
            // carry the inspect-thread owner onto worker threads so their pool
            // images stay attributed (instead of anonymous owner=0). Optional
            // symbol — older scripts don't export it and propagation is a no-op.
            if (g_eng.script.set_owner_callbacks) {
                g_eng.script.set_owner_callbacks((void*)owner_get_cb, (void*)owner_set_cb);
            }
            // Result callback. Scripts without xi_result.hpp leave this null
            // and xi::result() is a no-op (run_result then defaults to NA).
            if (g_eng.script.set_result_callback) {
                g_eng.script.set_result_callback((void*)result_cb);
            }
            // Replay any param values the user set on the previous
            // DLL. The new DLL's xi::Param<T> file-scope ctors run on
            // load and seed registry slots with default values; we
            // overwrite each one whose name we've seen via cmd:set_param
            // since the project opened. set_param returns -1 for
            // params the new DLL doesn't declare (renamed / deleted) —
            // those entries stay in the cache but quietly no-op until
            // the user hits set_param again, which is the right
            // failure mode (no false positives, no spurious errors).
            if (g_eng.script.set_param) {
                for (auto& [pname, pval] : g_eng.param_cache) {
                    g_eng.script.set_param(pname.c_str(), pval.c_str());
                }
                if (!g_eng.param_cache.empty()) {
                    std::fprintf(stderr,
                        "[xinsp2] replayed %zu param value(s) into reloaded script\n",
                        g_eng.param_cache.size());
                }
            }

            // Replay any script-instance defs the user tuned on the previous
            // DLL — exact sibling of the param replay above. The new DLL's
            // file-scope xi::Instance ctors re-seed each instance with its
            // SOURCE default def on load; without this the hot-recompile loop
            // silently reverts every operator-tuned/taught/calibrated instance.
            // set_instance_def returns non-zero for defs the new DLL doesn't
            // declare (renamed / deleted) — best-effort, like the param replay,
            // those entries stay cached and quietly no-op.
            if (g_eng.script.set_instance_def) {
                for (auto& [iname, def] : g_eng.instance_def_cache) {
                    // Replaying a cached def enters the freshly-swapped DLL's plugin
                    // code while we hold g_eng.run_mu + g_eng.script_mu. A plugin that throws
                    // (or faults, via the SEH translator) on an old/incompatible cached
                    // def must NOT terminate the backend mid-swap — log + skip it and
                    // keep replaying the rest. Do NOT touch the held locks in the catch.
                    try {
                        g_eng.script.set_instance_def(iname.c_str(), def.c_str());
                    } catch (const seh_exception& e) {
                        std::fprintf(stderr,
                            "[xinsp2] replay set_instance_def '%s' crashed: 0x%08X (%s) — skipped\n",
                            iname.c_str(), e.code, e.what());
                    } catch (const std::exception& e) {
                        std::fprintf(stderr,
                            "[xinsp2] replay set_instance_def '%s' threw: %s — skipped\n",
                            iname.c_str(), e.what());
                    }
                }
                if (!g_eng.instance_def_cache.empty()) {
                    std::fprintf(stderr,
                        "[xinsp2] replayed %zu instance def(s) into reloaded script\n",
                        g_eng.instance_def_cache.size());
                }
            }

            // Restore persistent state into the new DLL — but drop it
            // when the schema versions disagree (and both sides
            // declared one), since set_state's silent default-fill on
            // a shape change would confuse the new code more than
            // starting fresh would.
            if (g_eng.script.set_state && g_eng.persistent_state_json.size() > 2) {
                int new_schema = g_eng.script.state_schema_version
                               ? g_eng.script.state_schema_version()
                               : 0;
                // Drop whenever the NEW script declares a schema version that
                // differs from the persisted one — INCLUDING the 0→N case (a
                // script adopting versioning for the first time): the old
                // unversioned shape would otherwise default-fill into the new
                // shape, silently mis-shaping state. new_schema==0 (a script that
                // doesn't version) keeps the legacy "best-effort restore" path.
                bool drop = (new_schema != 0 &&
                             g_eng.persistent_state_schema != new_schema);
                if (drop) {
                    // G4 / OQ-5: before dropping, give the NEW DLL a chance to
                    // migrate the prior state forward via its opt-in code_change
                    // hook. If it carries state across the schema change, restore
                    // the migrated shape instead of dropping. Absent hook (or a
                    // decline) falls through to the unchanged drop path below.
                    std::string migrated;
                    if (xi::script::migrate_state(g_eng.script, g_eng.persistent_state_json,
                                                  g_eng.persistent_state_schema, new_schema,
                                                  migrated)) {
                        // set_state enters plugin code under g_eng.run_mu + g_eng.script_mu;
                        // a throwing/faulting plugin must not terminate the swap. On
                        // failure, drop the migrated state and continue (no lock touch).
                        bool state_ok = true;
                        try {
                            g_eng.script.set_state(migrated.c_str());
                        } catch (const seh_exception& e) {
                            state_ok = false;
                            std::fprintf(stderr,
                                "[xinsp2] replay set_state (migrated) crashed: 0x%08X (%s) — skipped\n",
                                e.code, e.what());
                        } catch (const std::exception& e) {
                            state_ok = false;
                            std::fprintf(stderr,
                                "[xinsp2] replay set_state (migrated) threw: %s — skipped\n", e.what());
                        }
                        if (!state_ok) { g_eng.persistent_state_json = "{}"; }
                        else {
                        g_eng.persistent_state_json = migrated;
                        std::fprintf(stderr,
                            "[xinsp2] state schema changed (v%d → v%d) — migrated prior state "
                            "(%zu bytes) via code_change hook\n",
                            g_eng.persistent_state_schema, new_schema, migrated.size());
                        std::string ev = "{\"type\":\"event\",\"name\":\"state_migrated\","
                                         "\"data\":{\"old_schema\":"
                                       + std::to_string(g_eng.persistent_state_schema)
                                       + ",\"new_schema\":"
                                       + std::to_string(new_schema)
                                       + "}}";
                        srv.send_text(ev);
                        g_eng.persistent_state_schema = new_schema;
                        }
                    } else {
                    std::fprintf(stderr,
                        "[xinsp2] state schema changed (v%d → v%d) — dropping prior state\n",
                        g_eng.persistent_state_schema, new_schema);
                    std::string ev = "{\"type\":\"event\",\"name\":\"state_dropped\","
                                     "\"data\":{\"old_schema\":"
                                   + std::to_string(g_eng.persistent_state_schema)
                                   + ",\"new_schema\":"
                                   + std::to_string(new_schema)
                                   + "}}";
                    srv.send_text(ev);
                    g_eng.persistent_state_json = "{}";
                    }
                } else {
                    // set_state enters plugin code under g_eng.run_mu + g_eng.script_mu; a
                    // throwing/faulting plugin must not terminate the swap — log + skip.
                    try {
                        g_eng.script.set_state(g_eng.persistent_state_json.c_str());
                        std::fprintf(stderr, "[xinsp2] state restored (%zu bytes, schema v%d)\n",
                                     g_eng.persistent_state_json.size(), new_schema);
                    } catch (const seh_exception& e) {
                        std::fprintf(stderr,
                            "[xinsp2] replay set_state (restore) crashed: 0x%08X (%s) — skipped\n",
                            e.code, e.what());
                    } catch (const std::exception& e) {
                        std::fprintf(stderr,
                            "[xinsp2] replay set_state (restore) threw: %s — skipped\n", e.what());
                    }
                }
            }
        }

        // Build log can be large — send as a log message, not inline data.
        if (!res.build_log.empty()) {
            xp::LogMsg lm;
            lm.level = "info";
            lm.msg = res.build_log;
            srv.send_text(lm.to_json());
        }

        // Re-arm continuous mode if it was running before the reload,
        // at the same fps the original cmd:start asked for. The 4 s
        // cl.exe gap inside the reload is unavoidable; what we don't
        // want is the run staying dead afterwards and forcing the
        // caller to know they need to re-issue cmd:start.
        // Install the trigger sink so a source's emit_trigger runs an inspect
        // even when NOT continuous (single-shot) — issue/replay works without
        // cmd:start. Continuous mode (below) just adds the free-running timer.
        install_trigger_sink_(&srv);
        // Preserve trigger-only mode across the reload (prior_continuous_fps == 0
        // means no timer — sources drive it). Same path as the error returns above.
        resume_continuous_if_needed();

        // P1-4: the swap succeeded — clear any prior degraded marker. The
        // "@compile" entry's seq/ts_ms double as a running-def generation+recency
        // stamp, so a client can tell a fresh good load from a stale "ok".
        set_status_internal("@compile", "ok");

        // Return success with dll path + diagnostics (warnings only on
        // success; extension still wants them for squiggle).
        std::string data = "{\"dll\":";
        xp::json_escape_into(data, res.dll_path);
        data += ",\"diagnostics\":" + build_diag_json();
        if (was_continuous) data += ",\"resumed_continuous\":true";
        data += "}";
        send_rsp_ok(srv, id, data);
}

void cmd_unload_script_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // P0-AB-1: dispatcher workers snapshot g_eng.script under
        // g_eng.script_mu and may be mid-inspect when unload_script
        // FreeLibrary's the DLL. Drain the pool first.
        { auto g = quiesce_dispatch_for_lifecycle_op_("unload_script", &srv); g.dismiss(); }  // script gone — don't resume
        std::lock_guard<std::mutex> lk(g_eng.script_mu);
        xi::script::unload_script(g_eng.script);
        // Drop the param replay cache — there's no live script to
        // replay into, and a future load_project / compile_and_load
        // is free to start clean.
        g_eng.param_cache.clear();
        g_eng.instance_def_cache.clear();   // sibling replay shadow — same lifetime
        send_rsp_ok(srv, id);
}

void cmd_save_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        auto path = xp::get_string_field(parsed->args_json, "path");
        if (!path) { send_rsp_err(srv, id, "missing path"); return; }
        std::string params_json, inst_json;
        {
            std::lock_guard<std::mutex> lk(g_eng.script_mu);
            if (g_eng.script.ok()) {
                std::vector<char> buf(64 * 1024);
                if (g_eng.script.list_params) {
                    int n = g_eng.script.list_params(buf.data(), (int)buf.size());
                    if (n > 0) params_json.assign(buf.data(), (size_t)n);
                }
                if (g_eng.script.list_instances) {
                    int n = g_eng.script.list_instances(buf.data(), (int)buf.size());
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
        for (auto& [iname, _] : g_eng.plugin_mgr.project().instances) {
            if (!g_eng.plugin_mgr.save_instance(iname)) save_fail = iname;
        }
        if (!save_fail.empty()) {
            // An instance config couldn't reach disk (disk full / read-only) — the
            // scratch we're about to commit is itself stale, so don't claim success.
            send_rsp_err(srv, id, "failed to persist instance '" + save_fail +
                         "' before commit (disk full / read-only?)");
        } else if (g_eng.plugin_mgr.commit_working_copy()) {
            send_rsp_ok(srv, id, "{\"committed\":true,\"canonical\":" +
                        ([]{ std::string s; xp::json_escape_into(s, g_eng.plugin_mgr.canonical_path()); return s; }()) + "}");
        } else {
            send_rsp_err(srv, id, "no working copy active (open with working_copy:true)");
        }
}

void cmd_discard_working_copy_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Blow away the scratch + re-seed from canonical, then reopen. Same
        // teardown constraint as open_project — drain the dispatch pool first.
        if (!g_eng.plugin_mgr.has_working_copy()) {
            send_rsp_err(srv, id, "no working copy active");
            return;
        }
        auto _wc_discard_guard = quiesce_dispatch_for_lifecycle_op_("discard_working_copy", &srv);  // resumes at block end
        if (g_eng.plugin_mgr.reopen_fresh_working_copy()) {
            send_rsp_ok(srv, id, g_eng.plugin_mgr.to_json());
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
            xp::Rsp r; r.id = id; r.ok = false; r.error = err;
            r.data_json = "{\"status\":\"rejected\"}";
            srv.send_text(r.to_json());
            push_recent_error("rsp", err, id);
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
                    if (g_eng.script.ok() && g_eng.script.set_param) {
                        int rc = g_eng.script.set_param(yyjson_get_str(nm), vbuf);
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
        // backend registry first, then fall through to g_eng.script.set_instance_def.
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
                        if (g_eng.script.ok() && g_eng.script.set_instance_def) {
                            int rc = g_eng.script.set_instance_def(iname, def_str);
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
            xp::Rsp r; r.id = id; r.ok = false;
            r.error = "project loaded only partially — see param_warnings/instance_warnings";
            r.data_json = data;
            srv.send_text(r.to_json());
            push_recent_error("rsp", r.error, id);
        } else {
            send_rsp_ok(srv, id, data);
        }
}

void cmd_create_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        auto folder = xp::get_string_field(parsed->args_json, "folder");
        auto pname  = xp::get_string_field(parsed->args_json, "name");
        if (!folder || !pname) { send_rsp_err(srv, id, "missing folder or name"); return; }
        if (g_eng.plugin_mgr.create_project(*folder, *pname)) {
            send_rsp_ok(srv, id, g_eng.plugin_mgr.to_json());
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
        bool working_copy = parsed->args_json.find("\"working_copy\":true") != std::string::npos
                          || parsed->args_json.find("\"working_copy\": true") != std::string::npos;
        { auto g = quiesce_dispatch_for_lifecycle_op_("open_project", &srv); g.dismiss(); }  // new project drives its own autostart
        // Drop stale bus state from any previously-open project (the old sink +
        // the per-source emit-time map, whose source names belong to the project
        // we're replacing) before tearing it down + opening the new one.
        xi::TriggerBus::instance().clear_sink();
        xi::TriggerBus::instance().reset();
        // Reset the script replay shadows on the PROJECT boundary, mirroring
        // unload_script's clear. open_project does NOT unload the inspection
        // script DLL (script lifecycle is independent of the project's plugin
        // DLLs), so without this the next project's compile_and_load would
        // (a) capture the PRIOR project's xi::state() into g_persistent_state_*
        // from the still-live old g_eng.script, then (b) replay the prior project's
        // g_eng.param_cache values over any same-named Param the new project
        // declares (e.g. "thresh") — running project B's inspections with
        // project A's tuned values / carried state and silently mis-verdicting.
        // A fresh project starts from its own file-scope defaults.
        {
            std::lock_guard<std::mutex> lk(g_eng.script_mu);
            g_eng.param_cache.clear();
            g_eng.instance_def_cache.clear();   // sibling replay shadow — same project boundary
            g_eng.persistent_state_json = "{}";
            g_eng.persistent_state_schema = 0;
        }
        if (g_eng.plugin_mgr.open_project(*folder, working_copy)) {
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
            auto& proj = g_eng.plugin_mgr.project();
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
            auto warns = g_eng.plugin_mgr.open_warnings();
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
            resolve_toolchain_(*folder);
            // Put the project folder on the DLL search path so a script's
            // statically-linked external dep DLL can live in the project folder.
            set_project_dll_search_(*folder);
            // (IDE IntelliSense config: the VS Code extension writes
            // <project>/.vscode/c_cpp_properties.json itself, reading the compile
            // paths via cmd:toolchain_health — the core no longer touches .vscode.)
            send_rsp_ok(srv, id, g_eng.plugin_mgr.to_json());
        } else {
            // Prefer a specific hard-refusal reason (e.g. an unrecognized future
            // project-file schema) over the generic message.
            std::string oe = g_eng.plugin_mgr.open_error();
            send_rsp_err(srv, id, !oe.empty() ? oe : ("failed to open project in " + *folder));
        }
}

void cmd_close_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // P0-AB-3: must drain dispatch pool BEFORE close_project tears
        // down instances and FreeLibrary's plugin DLLs. (PR #33 fixed
        // the in-PluginManager teardown order; this fixes the
        // dispatcher-still-running case the dispatcher pool hit when
        // close_project is sent during continuous mode.)
        { auto g = quiesce_dispatch_for_lifecycle_op_("close_project", &srv); g.dismiss(); }  // project closed — nothing to stream
        // Drop the bus's captured sink (it points at `srv`) BEFORE the plugin
        // DLLs are unloaded — otherwise the stale sink can fire into a torn-down
        // project. reset() also prunes the per-source emit-time map, whose source
        // names belong to the project being closed (otherwise they accumulate
        // across every open→emit→close cycle).
        xi::TriggerBus::instance().clear_sink();
        xi::TriggerBus::instance().reset();
        g_eng.plugin_mgr.close_project();
        clear_inst_state();   // instances are gone — drop host-tracked state
        // Reset the script replay shadows on the PROJECT boundary, mirroring
        // unload_script's clear. Closing a project doesn't unload the script
        // DLL, but the operator-tuned param cache + persisted xi::state() belong
        // to the project just closed — leaving them in place would leak A's
        // values/state into whatever project is opened next (see open_project).
        {
            std::lock_guard<std::mutex> lk(g_eng.script_mu);
            g_eng.param_cache.clear();
            g_eng.instance_def_cache.clear();   // sibling replay shadow — same project boundary
            g_eng.persistent_state_json = "{}";
            g_eng.persistent_state_schema = 0;
        }
        send_rsp_ok(srv, id, "{\"closed\":true}");
}

