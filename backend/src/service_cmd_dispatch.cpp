//
// service_cmd_dispatch.cpp — dispatch-control command handlers, split from
// service_main.cpp (behavior-preserving; see service_internal.hpp).
//
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <yyjson.h>

#include "service_internal.hpp"

// ---- dispatch-control ------------------------------------------------------
void cmd_set_timer_fps_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Live synthetic-tick rate. fps <= 0 = trigger-only (no ticks). Takes
        // effect on the next timer loop while continuous mode is running; persisted
        // by the UI to project.json runtime.timer_fps.
        auto f = xp::get_number_field(parsed->args_json, "fps");
        int fps = f ? (int)*f : 0;
        // max(1,..) so a high fps (>1000) doesn't round to 0, which the timer
        // loop reads as "off" (the opposite of what was asked). fps<=0 = off.
        int iv = fps > 0 ? std::max(1, 1000 / fps) : 0;
        g_eng.timer_interval_ms.store(iv);
        std::string out = "{\"fps\":" + std::to_string(fps) +
                          ",\"interval_ms\":" + std::to_string(iv) + "}";
        send_rsp_ok(srv, id, out);
}

void cmd_run_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        if (g_eng.continuous.load()) {
            send_rsp_err(srv, id, "cannot run while continuous mode is active — stop first");
            return;
        }
        // No script loaded: return a clear error NOW, before the ok+detached-run
        // path. `run` sends its rsp before vars, so without this a no-script run
        // would reply ok, then silently emit no vars — a headless driver waiting
        // for vars times out with an empty error and drops the WS. open_project
        // does not compile the project's script (that's compile_and_load's job),
        // so this is the common headless gotcha. (Reported bug BUG-3.)
        if (!g_eng.script.ok()) {
            send_rsp_err(srv, id, "no script loaded — call compile_and_load first");
            return;
        }
        int64_t run_id = ++g_eng.run_id;

        // Optional `frame_path` arg — plumbed to the script via
        // `xi::current_frame_path()`. Was previously parsed by tests /
        // SDKs but ignored by this handler ("phantom argument"). Now
        // wired end to end.
        std::string frame_path;
        if (auto fp = xp::get_string_field(parsed->args_json, "frame_path")) {
            frame_path = *fp;
        }

        // Stage 1b — optional inline `meta` object: its raw JSON is parsed into a
        // metadata doc and injected into this run's current_trigger() so a
        // headless cmd:run feeds the script the same record (frame image + meta)
        // a source's emit_record would, with no source plugin needed.
        std::string meta_json;
        {
            std::string m; const char* after = nullptr;
            if (xp::detail::find_key(parsed->args_json.data(),
                                     parsed->args_json.data() + parsed->args_json.size(),
                                     "meta", m, after)) {
                meta_json = std::move(m);
            }
        }

        // Send rsp first (tests expect rsp before vars).
        char buf[128];
        std::snprintf(buf, sizeof(buf), R"({"run_id":%lld,"ms":0})", (long long)run_id);
        send_rsp_ok(srv, id, buf);

        // Run inspection on a detached thread so a long inspect doesn't block
        // the WS poll loop (and so the watchdog can observe its deadline slot).
        // Serialised on g_eng.run_mu so 8 quick `cmd:run` calls produce
        // vars entries in run_id order.
        // SEH translator must be installed inside the thread.
        //
        // cmd:run is INTENTIONALLY serial — it's the deterministic single-shot
        // path (UI "Run", driver step-through) and is rejected outright while
        // continuous mode is active (above). Burst/throughput parallelism is the
        // continuous-mode dispatch pool's job (parallelism.dispatch_threads +
        // emit_trigger / fps); fanning out cmd:run would break this run_id-order
        // contract for no real burst gain (bursts arrive via the trigger path).
        crash_set(crash_ctx().last_cmd, sizeof(crash_ctx().last_cmd), "run");
        crash_ctx().last_run_id = (int)run_id;
        // The detached thread dereferences the main-local srv, so g_eng.inflight owns
        // the bump/bail/drain; teardown waits it out. A launch racing shutdown (or a
        // spawn failure) just runs nothing — there's no rsp for an async run anyway.
        g_eng.inflight.launch([&srv, run_id,
                     frame_path = std::move(frame_path),
                     meta_json  = std::move(meta_json)]() {
            reserve_fault_stack();   // BUG 2: dump survives a script stack overflow
            xi::install_seh_translator();
            std::lock_guard<std::mutex> lk(g_eng.run_mu);

            // Stage 1b: build a one-shot record (frame image + meta) and expose
            // it as this run's current_trigger — the same path the dispatch
            // worker uses (thread_local g_current_trigger). Only injected when
            // there's something to inject, so a plain cmd:run keeps the previous
            // "no trigger" behaviour (current_trigger().is_active() == false).
            xi::TriggerEvent ev;
            bool inject = false;
            if (!frame_path.empty()) {
                if (auto fn = xi::ImagePool::read_image_file_fn()) {
                    if (xi_image_handle h = fn(frame_path.c_str())) {
                        ev.images["frame"] = h;   // read under current_trigger().image("frame")
                        inject = true;
                    }
                }
            }
            if (!meta_json.empty()) {
                if (yyjson_doc* idoc = yyjson_read(meta_json.data(), meta_json.size(), 0)) {
                    yyjson_mut_doc* meta = yyjson_doc_mut_copy(idoc, nullptr);
                    yyjson_doc_free(idoc);
                    if (meta) {
                        xi::DocRegistry::instance().addref(meta);   // register at rc=1
                        ev.meta_doc = xi::DocRef::adopt(meta);
                        inject = true;
                    }
                }
            }
            if (inject) {
                ev.id = { (uint64_t)run_id, 0 };   // synthesized, unique per run
                CurrentTriggerScope trig(ev);      // clears g_current_trigger + releases ev on scope exit
                run_one_inspection(srv, /*frame_hint=*/1, run_id, frame_path);
            } else {
                run_one_inspection(srv, /*frame_hint=*/1, run_id, frame_path);
            }
        });
}

void cmd_start_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Start continuous trigger mode. The backend runs a timer thread
        // that calls inspect() at a configurable interval. The script's
        // own ImageSource (if any) runs its acquisition thread inside
        // the DLL — the backend doesn't manage it.
        if (g_eng.continuous.load()) {
            send_rsp_ok(srv, id, R"({"already":true})");
            return;
        }

        // Parse optional fps from args (default 10). fps <= 0 means TRIGGER-ONLY:
        // start continuous (spawn the lanes) but run NO synthetic timer tick — the
        // project's sources are the only dispatch driver. (Avoids loading the
        // default group with timer ticks; see docs/internals/dispatch.md.)
        // fps here is the SYNTHETIC-TIMER-TICK rate, NOT a real inspection driver —
        // see "CONTINUOUS RUN HAS TWO DRIVERS" at the top of this file. fps>0 ticks
        // a source-less script; fps<=0 = trigger-only (sources drive, the normal
        // case). An EXPLICIT fps seeds the live timer rate; if absent, keep whatever
        // g_eng.timer_interval_ms already holds (project.json runtime.timer_fps, a prior
        // set_timer_fps, or the default 10fps) — so a project's saved pref isn't
        // clobbered by a bare start.
        int  fps = 10;
        bool trigger_only = false;
        bool fps_explicit = false;
        auto fps_val = xp::get_number_field(parsed->args_json, "fps");
        if (fps_val) {
            fps_explicit = true;
            // Clamp the WS-supplied double before the cast: (int)1e300 is UB.
            if (*fps_val > 0) fps = (int)std::min(*fps_val, 100000.0);
            else trigger_only = true;
        }

        // Stop any existing pool before starting a new one. (A-P1-2: any events
        // that arrived since the last stop are drained + their handles released
        // inside stop_group_pool_, so the new run never fires on stale images.)
        if (g_eng.timer_thread.joinable()) {
            stop_dispatch_pool_();
        }

        g_eng.continuous_fps = trigger_only ? 0 : fps;
        g_eng.continuous = true;

        // Seed the live timer rate (0 = trigger-only). Only when fps was explicit;
        // otherwise keep the existing g_eng.timer_interval_ms (runtime/prior/default).
        if (fps_explicit) g_eng.timer_interval_ms.store(trigger_only ? 0 : std::max(1, 1000 / std::max(fps, 1)));
        int interval_ms = g_eng.timer_interval_ms.load();

        // Bus-driven dispatch: with g_eng.continuous now true the sink enqueues to
        // the worker pool (single-shot otherwise). Timer thread emits synthetic
        // events on schedule for scripts without trigger sources.
        install_trigger_sink_(&srv);
        spawn_group_pool_(&srv, interval_ms);

        // The watchdog now tracks a per-inspect slot, so it protects every
        // worker under N>1 (no longer bypassed). On a hard trip the backend
        // exits for the FE to respawn; under N>1 the cooperative-cancel phase
        // is global (aborts all in-flight frames that round). See
        // run_one_inspection() + docs/guides/write-a-script.md.

        int n_threads = std::max(1, g_eng.plugin_mgr.project().dispatch_threads);
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      R"({"started":true,"dispatch_threads":%d})", n_threads);
        send_rsp_ok(srv, id, buf);
}

void cmd_stop_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        g_eng.continuous = false;
        xi::TriggerBus::instance().clear_sink();
        stop_dispatch_pool_();   // joins lanes + drains their queues (handles released)
        send_rsp_ok(srv, id, R"({"stopped":true})");
}

void cmd_exchange_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Crash-blame: capture which instance/plugin we're about to talk to.
        if (auto in = xp::get_string_field(parsed->args_json, "name")) {
            crash_set(crash_ctx().last_cmd, sizeof(crash_ctx().last_cmd), "exchange_instance");
            crash_set(crash_ctx().last_instance, sizeof(crash_ctx().last_instance), in->c_str());
            if (auto inst = xi::InstanceRegistry::instance().find(*in)) {
                crash_set(crash_ctx().last_plugin, sizeof(crash_ctx().last_plugin),
                          inst->plugin_name().c_str());
                // G2.1 — exchange() also enters plugin code; attribute a fault here.
                stamp_culprit_(in->c_str(), inst->plugin_name());
            }
        }
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        std::string cmd_str;
        const char* after;
        if (xp::detail::find_key(parsed->args_json.data(),
                                  parsed->args_json.data() + parsed->args_json.size(),
                                  "cmd", cmd_str, after)) {
        } else {
            cmd_str = "{}";
        }
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        if (inst) {
            try {
                std::string result = inst->exchange(cmd_str);
                send_rsp_ok(srv, id, result);
            } catch (const seh_exception& e) {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "exchange '%s' crashed: 0x%08X (%s)",
                             iname->c_str(), e.code, e.what());
                send_rsp_err(srv, id, msg);
            } catch (const std::exception& e) {
                send_rsp_err(srv, id, std::string("exchange error: ") + e.what());
            }
        } else {
            std::lock_guard<std::mutex> lk(g_eng.script_mu);
            if (g_eng.script.ok() && g_eng.script.exchange_instance) {
                try {
                    std::vector<char> rsp(256 * 1024);
                    int n = g_eng.script.exchange_instance(iname->c_str(), cmd_str.c_str(),
                                                       rsp.data(), (int)rsp.size());
                    if (n < 0) { rsp.resize((size_t)(-(int64_t)n) + 1024);
                                 n = g_eng.script.exchange_instance(iname->c_str(), cmd_str.c_str(),
                                                                rsp.data(), (int)rsp.size()); }
                    if (n >= 0) send_rsp_ok(srv, id, std::string(rsp.data(), (size_t)n));
                    else        send_rsp_err(srv, id, "exchange_instance failed");
                } catch (const seh_exception& e) {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg), "script exchange '%s' crashed: 0x%08X (%s)",
                                 iname->c_str(), e.code, e.what());
                    send_rsp_err(srv, id, msg);
                }
            } else {
                send_rsp_err(srv, id, "instance not found: " + *iname);
            }
        }
}

void cmd_prepare_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Orchestrator STAGE (ABI v7, task #69): load a new config's heavy assets
        // into an instance's BACKGROUND staging slot, off the critical path — the
        // live config keeps running. Pair with commit_group to swap them in
        // frame-perfectly. For a plugin that opted into XI_PLUGIN_STAGED this calls
        // its ungated prepare() (concurrent with process); otherwise it falls back
        // to a gated set_def (immediate swap — the tier-1 path). Script-side
        // instances keep the exchange convention.
        // args: { "name": "cam0", "def": { ... }, "folder"?: "..." }
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        std::string def_str;
        const char* after;
        if (!xp::detail::find_key(parsed->args_json.data(),
                                  parsed->args_json.data() + parsed->args_json.size(),
                                  "def", def_str, after)) {
            def_str = "{}";
        }
        auto folder = xp::get_string_field(parsed->args_json, "folder");
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        if (inst) {
            bool ok = false;
            try { ok = inst->prepare(def_str, folder ? *folder : std::string()); }
            catch (const std::exception& e) {
                set_inst_state(*iname, InstState::Faulted, e.what());
                send_rsp_err(srv, id, std::string("prepare error: ") + e.what());
                return;
            }
            if (ok) send_rsp_ok(srv, id);
            else { set_inst_state(*iname, InstState::Faulted, "prepare returned false");
                   send_rsp_err(srv, id, "prepare returned false"); }
        } else {
            // Script-side: exchange convention {command:"prepare", def, folder}.
            std::string cmd = "{\"command\":\"prepare\",\"def\":" + def_str;
            if (folder) { cmd += ",\"folder\":"; xp::json_escape_into(cmd, *folder); }
            cmd += "}";
            std::lock_guard<std::mutex> lk(g_eng.script_mu);
            if (g_eng.script.ok() && g_eng.script.exchange_instance) {
                // Script-side prepare enters plugin code — guard like the backend
                // path above (and exchange_instance) so a throw/fault isn't fatal.
                try {
                    std::vector<char> buf(64 * 1024);
                    int n = g_eng.script.exchange_instance(iname->c_str(), cmd.c_str(),
                                                       buf.data(), (int)buf.size());
                    if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024);
                                 n = g_eng.script.exchange_instance(iname->c_str(), cmd.c_str(),
                                                                buf.data(), (int)buf.size()); }
                    if (n >= 0) send_rsp_ok(srv, id, std::string(buf.data(), (size_t)n));
                    else        send_rsp_err(srv, id, "prepare failed");
                } catch (const seh_exception& e) {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg), "script prepare '%s' crashed: 0x%08X (%s)",
                                 iname->c_str(), e.code, e.what());
                    send_rsp_err(srv, id, msg);
                } catch (const std::exception& e) {
                    send_rsp_err(srv, id, std::string("script prepare error: ") + e.what());
                }
            } else {
                send_rsp_err(srv, id, "instance not found: " + *iname);
            }
        }
}

void cmd_commit_group_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Orchestrator DRAIN-BARRIER (RFC #65 / config-swap design, tasks #66/#69).
        // Commit a GROUP of instances atomically w.r.t. inspection runs: quiesce
        // dispatch + drain in-flight runs so NO process() is mid-flight, call the
        // first-class commit() slot on every target in that one no-process window
        // (so no run ever sees a half-committed group), then resume dispatch at the
        // prior fps — a config switch must not stop the camera stream. Reuses the
        // same quiesce primitive as recompile/rebuild. The expensive asset load has
        // already happened off the barrier via `prepare_instance`; commit() is just
        // a cheap pointer swap, so this barrier is one in-flight run (~ms).
        //
        // Addressing (task #68): an explicit name array AND/OR selectors that the
        // host expands against existing instance properties — no new schema:
        //   "instances": ["a","b"]   explicit names (covers script-side too)
        //   "group":  "line1"        all backend instances in that dispatch group
        //   "plugin": "binarize"     all backend instances of that plugin type
        // The union is deduped. (If config-switch cohorts ever need to cut ACROSS
        // dispatch groups, add a dedicated per-instance tag then; reusing `group`
        // + `plugin` is the zero-schema choice that covers the common cases.)
        // args: { instances?, group?, plugin? }
        // See docs/roadmap/config-bundles-and-orchestration.md.
        std::vector<std::string> targets;
        std::unordered_set<std::string> seen;
        auto add_target = [&](const std::string& n) {
            if (seen.insert(n).second) targets.push_back(n);
        };
        if (yyjson_doc* adoc = yyjson_read(parsed->args_json.c_str(),
                                           parsed->args_json.size(), 0)) {
            yyjson_val* arr = yyjson_obj_get(yyjson_doc_get_root(adoc), "instances");
            if (yyjson_is_arr(arr)) {
                size_t _i, _n; yyjson_val* it;
                yyjson_arr_foreach(arr, _i, _n, it) {
                    const char* s = yyjson_get_str(it);
                    if (yyjson_is_str(it) && s) add_target(s);
                }
            }
            yyjson_doc_free(adoc);
        }
        auto group_sel  = xp::get_string_field(parsed->args_json, "group");
        auto plugin_sel = xp::get_string_field(parsed->args_json, "plugin");
        if (group_sel || plugin_sel) {
            for (auto& [iname, ii] : g_eng.plugin_mgr.project().instances) {
                if (group_sel  && ii.group       != *group_sel)  continue;
                if (plugin_sel && ii.plugin_name != *plugin_sel) continue;
                add_target(iname);
            }
        }
        if (targets.empty()) {
            send_rsp_err(srv, id, "no targets — pass instances[], group, or plugin");
            return;
        }
        // DRAIN-BARRIER: after this returns there is provably no process() running
        // (pool stopped + workers joined + in-flight cmd:run drained via g_eng.run_mu).
        auto guard = quiesce_dispatch_for_lifecycle_op_("commit_group", &srv);
        std::string results = "[";
        bool any_fail = false;
        for (size_t i = 0; i < targets.size(); ++i) {
            if (i) results += ",";
            results += "{\"name\":"; xp::json_escape_into(results, targets[i]);
            std::string r; bool ok = false;
            auto inst = xi::InstanceRegistry::instance().find(targets[i]);
            if (inst) {
                // First-class commit() slot (ABI v7): swap staging → live. The
                // result echoes the now-live def. A plugin with no double-slot
                // gets the InstanceBase no-op (it already swapped in set_def).
                try { inst->commit(); r = inst->get_def(); ok = true; }
                catch (const std::exception& e) {
                    r = std::string("{\"error\":\"") + e.what() + "\"}";
                }
            } else {
                // Script-side instances keep the exchange convention.
                std::lock_guard<std::mutex> lk(g_eng.script_mu);
                if (g_eng.script.ok() && g_eng.script.exchange_instance) {
                    // Script-side commit enters plugin code — guard like the backend
                    // inst->commit() path above so a throw/fault isn't fatal (record
                    // it as a per-target failure and keep committing the rest).
                    try {
                        const char* commit_cmd = R"({"command":"commit"})";
                        std::vector<char> buf(64 * 1024);
                        int n = g_eng.script.exchange_instance(targets[i].c_str(), commit_cmd,
                                                           buf.data(), (int)buf.size());
                        if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024);
                                     n = g_eng.script.exchange_instance(targets[i].c_str(), commit_cmd,
                                                                    buf.data(), (int)buf.size()); }
                        if (n >= 0) { r.assign(buf.data(), (size_t)n); ok = true; }
                    } catch (const seh_exception& e) {
                        char em[256];
                        std::snprintf(em, sizeof(em), "{\"error\":\"commit crashed: 0x%08X\"}", e.code);
                        r = em;
                    } catch (const std::exception& e) {
                        r = std::string("{\"error\":\"") + e.what() + "\"}";
                    }
                }
                if (!ok) r = "{\"error\":\"instance not found\"}";
            }
            if (!ok) any_fail = true;
            set_inst_state(targets[i], ok ? InstState::Active : InstState::Faulted,
                           ok ? "" : "commit failed");
            results += ",\"ok\":"; results += ok ? "true" : "false";
            results += ",\"result\":"; results += r.empty() ? "null" : r;
            results += "}";
        }
        results += "]";
        // `guard` resumes dispatch at the prior fps when it goes out of scope at
        // the end of this handler (config switch must not halt streaming).
        std::string data = "{\"results\":" + results + "}";
        if (any_fail) {
            xp::Rsp r; r.id = id; r.ok = false;
            r.error = "one or more commits failed"; r.data_json = data;
            srv.send_text(r.to_json());
        } else {
            send_rsp_ok(srv, id, data);
        }
}

