//
// service_cmd_script.cpp — SCRIPT lifecycle command handler: cmd:compile_and_load
// (compile-or-load the script .dll, then the run_mu+script_mu hot-swap that makes
// it the active script) plus the cross-frame state carried across that swap —
// param / instance-def replay and the persistent-kv capture / migrate / restore
// (restore_kv_into_new_script_). Split from the former service_cmd_lifecycle.cpp
// (behavior-preserving code motion).
//
// Deliberately NOT here: the PROJECT + working-copy lifecycle (open/close/create/
// save/load_project, commit/discard_working_copy) and the ping/version/shutdown
// session handlers live in service_cmd_project_lc.cpp. The guarded_script_call
// SEH/exception fault boundary this file uses lives in service_guard.hpp.
//
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <xi/xi_kv.hpp>      // round-3 #7: kKvHardCapBytes (capture-site bound)
#include <xi/xi_script_compiler.hpp>

#include "service_cmds.hpp"
#include "service_guard.hpp"          // guarded_script_call / ScriptCallOutcome (kv replay under the SEH guard)
#include <xi/xi_health.hpp>            // IWYU: xi::health()/CompHealth (formerly transitive via service_state.hpp)
#include <xi/xi_script_loader.hpp>    // IWYU: LoadedScript members (formerly transitive via service_state.hpp; now pimpl-hidden)
#include <xi/xi_ws_server.hpp>

// ---- script hot-swap: compile, swap-in, cross-frame state migration --------

// U2 (docs/new_gen/16): restore the captured kv channel into the freshly-swapped
// script DLL — drop when the schema versions disagree (and the new side declared
// one), consult the opt-in kv_change migrate hook first, all under the shared
// guarded_script_call SEH/exception guard (round-3 W2 #6 extraction: this block
// plus its two hand-rolled catch ladders lived inline in cmd_compile_and_load_).
// Events use the state_migrated/state_dropped names with a "store":"kv"
// discriminator. Caller holds g_eng.run_mu + g_eng.script_mu (the swap block);
// no-op when the new DLL has no kv channel or nothing was captured.
static void restore_kv_into_new_script_(xi::ws::Server& srv) {
    if (!g_eng.script().set_kv || g_eng.persistent_kv_bytes.empty()) return;
    int new_kv_schema = g_eng.script().kv_schema_version
                      ? g_eng.script().kv_schema_version()
                      : 0;
    // Downgrade hole: a new script that LOST its XI_KV_SCHEMA decl
    // (new_kv_schema == 0) while the captured store WAS versioned
    // must also drop — we can't distinguish "never versioned" from
    // "version decl lost", so the safe choice is drop rather than
    // blind-restoring a versioned store into an unversioned script.
    // 0 → 0 (never versioned on either side) keeps the blind
    // best-effort restore.
    bool kv_downgrade = (g_eng.persistent_kv_schema != 0 && new_kv_schema == 0);
    bool kv_drop = (new_kv_schema != 0 &&
                    g_eng.persistent_kv_schema != new_kv_schema)
                 || kv_downgrade;
    if (kv_drop) {
        std::string kv_migrated;
        if (xi::script::migrate_kv(g_eng.script(), g_eng.persistent_kv_bytes,
                                   g_eng.persistent_kv_schema, new_kv_schema,
                                   kv_migrated)) {
            auto oc = guarded_script_call(
                "replay kv_set (migrated)", "replay kv_set (migrated)",
                [&] { return g_eng.script().set_kv((const uint8_t*)kv_migrated.data(),
                                                 (int)kv_migrated.size()); });
            if (oc == ScriptCallOutcome::Refused)
                std::fprintf(stderr,
                    "[xinsp2] replay kv_set (migrated) refused the bytes — skipped\n");
            if (oc != ScriptCallOutcome::Ok) {
                g_eng.persistent_kv_bytes.clear();
            } else {
                g_eng.persistent_kv_bytes = kv_migrated;
                std::fprintf(stderr,
                    "[xinsp2] kv schema changed (v%d → v%d) — migrated prior store "
                    "(%zu bytes) via kv_change hook\n",
                    g_eng.persistent_kv_schema, new_kv_schema, kv_migrated.size());
                std::string ev = "{\"type\":\"event\",\"name\":\"state_migrated\","
                                 "\"data\":{\"store\":\"kv\",\"old_schema\":"
                               + std::to_string(g_eng.persistent_kv_schema)
                               + ",\"new_schema\":"
                               + std::to_string(new_kv_schema)
                               + "}}";
                srv.send_text(ev);
                g_eng.persistent_kv_schema = new_kv_schema;
            }
        } else {
            std::fprintf(stderr,
                "[xinsp2] kv schema changed (v%d → v%d) — dropping prior store\n",
                g_eng.persistent_kv_schema, new_kv_schema);
            std::string ev = "{\"type\":\"event\",\"name\":\"state_dropped\","
                             "\"data\":{\"store\":\"kv\",\"old_schema\":"
                           + std::to_string(g_eng.persistent_kv_schema)
                           + ",\"new_schema\":"
                           + std::to_string(new_kv_schema)
                           + ",\"reason\":\""
                           + (kv_downgrade ? "schema_downgrade" : "schema_mismatch")
                           + "\"}}";
            srv.send_text(ev);
            g_eng.persistent_kv_bytes.clear();
        }
    } else {
        // A refused/faulted restore loses the whole cross-frame
        // store while compile_and_load still reports success —
        // surface it as the same state_dropped event the schema
        // drop path emits, so a headless controller can detect
        // the loss (stderr alone is invisible over WS).
        auto kv_restore_dropped = [&](const char* reason) {
            std::string ev = "{\"type\":\"event\",\"name\":\"state_dropped\","
                             "\"data\":{\"store\":\"kv\",\"old_schema\":"
                           + std::to_string(g_eng.persistent_kv_schema)
                           + ",\"new_schema\":"
                           + std::to_string(new_kv_schema)
                           + ",\"reason\":\"" + reason + "\"}}";
            srv.send_text(ev);
        };
        auto oc = guarded_script_call(
            "replay kv_set (restore)", "replay kv_set (restore)",
            [&] { return g_eng.script().set_kv((const uint8_t*)g_eng.persistent_kv_bytes.data(),
                                             (int)g_eng.persistent_kv_bytes.size()); });
        switch (oc) {
        case ScriptCallOutcome::Ok:
            std::fprintf(stderr, "[xinsp2] kv restored (%zu bytes, schema v%d)\n",
                         g_eng.persistent_kv_bytes.size(), new_kv_schema);
            break;
        case ScriptCallOutcome::Refused:
            std::fprintf(stderr,
                "[xinsp2] replay kv_set (restore) refused the bytes — skipped\n");
            kv_restore_dropped("restore_refused");
            break;
        case ScriptCallOutcome::Crashed:
        case ScriptCallOutcome::Threw:
            kv_restore_dropped("restore_faulted");
            break;
        }
    }
}

void cmd_compile_and_load_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        auto src = xp::get_string_field(parsed->args_json, "path");
        if (!src) {
            send_rsp_err(srv, id, "compile_and_load: missing path");
            return;
        }

        // Quiesce dispatch for the script-DLL swap via the structural guard —
        // the same primitive every other lifecycle handler holds. Root cause
        // (quiesce-ritual family, RT5/O2 kin): this handler used to hand-roll
        // stop_dispatch_pool_() + a resume lambda called at each of 4 exits; a
        // throw between stop and resume (or a 5th exit added later) left the
        // production stream permanently stopped. The guard's DESTRUCTOR resumes
        // continuous mode at the prior fps on EVERY exit, exceptions included.
        // Deliberate deltas vs. the old ritual (both safe-side): the guard also
        // (a) pauses detached one-shot launches + clears the bus sink for the
        // whole compile window — a source-emit one-shot during the ~4 s compile
        // is DROPPED instead of racing the swap (matching open/close/recompile)
        // — and (b) drains in-flight runs up front (the swap block below still
        // takes run_mu+script_mu itself, unchanged). The dtor unpauses,
        // re-installs the sink it removed, and respawns continuous mode.
        auto reload_guard = quiesce_dispatch_for_lifecycle_op_("compile_and_load", &srv);

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
                xi::health().set_script(xi::CompHealth::Failed, xi::kReasonCompileError,
                                        std::filesystem::path(*src).filename().string());
                return;   // reload_guard's dtor re-arms continuous mode
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
            // Wave-2 #2: the data-carrying send_rsp_err overload owns the
            // recent-errors push — this site used to hand-roll the Rsp and
            // FORGOT it, so a failed compile never showed in cmd:recent_errors.
            send_rsp_err(srv, id, "compile failed",
                         "{\"diagnostics\":" + build_diag_json() + "}");
            xp::LogMsg lm;
            lm.level = "error";
            lm.msg = res.build_log;
            srv.send_text(lm.to_json());
            // P1-4: the rsp ok:false only reaches THIS caller; publish a sticky
            // "@compile" marker so a later status poll (or a reconnecting operator)
            // can tell the line is running the last-good def in a degraded state.
            set_status_internal("@compile", "degraded: compile failed");
            xi::health().set_script(xi::CompHealth::Failed, xi::kReasonCompileError,
                                    std::filesystem::path(*src).filename().string());
            return;   // reload_guard's dtor keeps streaming the last-good script
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
                xi::health().set_script(xi::CompHealth::Failed, xi::kReasonCompileError,
                                        std::filesystem::path(*src).filename().string());
                return;   // reload_guard's dtor resumes — old g_eng.script() untouched, keeps streaming
            }
            // U2 (docs/new_gen/16): capture the kv channel from the OLD DLL
            // before swapping it out — canonical-mp bytes, explicit lengths
            // (0 = empty store, nothing kept). Schema stamped alongside so
            // restore into the new DLL can detect a shape mismatch.
            if (g_eng.script().ok() && g_eng.script().get_kv) {
                std::vector<uint8_t> kbuf(256 * 1024);
                // Round-3 #7: xi::kKvHardCapBytes is enforced HERE — the one
                // edge where the serialized store crosses into host memory and
                // is persisted across reloads (see the rationale at the
                // constant in xi_kv.hpp; per-set SDK checks would change
                // script-side semantics and can't see the aggregate size).
                // The -N "need N" probe reply is checked BEFORE the grow, so an
                // oversized store is refused without the host ever allocating
                // it. On refusal the capture is DROPPED and the PREVIOUS
                // snapshot (bytes + schema) is kept — losing this reload's
                // delta is strictly better than buffering an unbounded blob
                // under run_mu + script_mu.
                auto kv_oversized = [&](int64_t sz) {
                    std::fprintf(stderr,
                        "[xinsp2] kv capture DROPPED: serialized store is %lld bytes, "
                        "over the %d-byte hard cap — keeping the previous snapshot "
                        "(kv is cross-frame state, not bulk storage)\n",
                        (long long)sz, xi::kKvHardCapBytes);
                    std::string ev = "{\"type\":\"event\",\"name\":\"state_dropped\","
                                     "\"data\":{\"store\":\"kv\",\"reason\":\"oversized\","
                                     "\"size\":" + std::to_string(sz) +
                                     ",\"cap\":" + std::to_string(xi::kKvHardCapBytes) + "}}";
                    srv.send_text(ev);
                };
                // kv_get has no -1 error return (0 = empty store; -N = need N).
                int kn = g_eng.script().get_kv(kbuf.data(), (int)kbuf.size());
                if (kn < 0 && -(int64_t)kn > xi::kKvHardCapBytes) {
                    kv_oversized(-(int64_t)kn);
                } else {
                    if (kn < 0) {   // grow-retry (script_grow_retry shape, now cap-guarded)
                        kbuf.resize((size_t)(-(int64_t)kn) + 1024);
                        kn = g_eng.script().get_kv(kbuf.data(), (int)kbuf.size());
                    }
                    if (kn > xi::kKvHardCapBytes) {   // grew past the cap between probes
                        kv_oversized(kn);
                    } else {
                        if (kn > 0) g_eng.persistent_kv_bytes.assign((const char*)kbuf.data(), (size_t)kn);
                        else        g_eng.persistent_kv_bytes.clear();
                        g_eng.persistent_kv_schema = g_eng.script().kv_schema_version
                                                   ? g_eng.script().kv_schema_version()
                                                   : 0;
                    }
                }
            } else {
                // No kv channel on the OLD script (or no old script at all):
                // disarm any snapshot captured in an earlier era. Without this,
                // a script that stopped exporting get_kv leaves the previous
                // capture armed, and a LATER set_kv-bearing DLL silently
                // resurrects a weeks-old store.
                g_eng.persistent_kv_bytes.clear();
                g_eng.persistent_kv_schema = 0;
            }
            // Swap: move-assign drops the old module's last ref — its deleter
            // does the owner-sweep + FreeLibrary once any in-flight inspect that
            // snapshotted it returns.
            g_eng.script() = std::move(next);
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
            if (g_eng.script().set_use_callbacks) {
                g_eng.script().set_use_callbacks(
                    nullptr,   // THE CUT: the Record use()->process bridge is gone
                    (void*)use_exchange_cb,
                    nullptr,   // grab_fn slot retained in the ABI; SDK discards it
                    (void*)script_host_api_());
            }
            // polaris2 Gate P2: pack-door process callback, so scripts can
            // chain a Pack (t.pack() or a built one) into a plugin's xi.pack@1
            // door via xi::use(name).process(pack). Optional symbol — scripts
            // compiled before it simply don't export it and the ScriptPack
            // overload returns an empty pack.
            if (g_eng.script().set_use_pack_callback) {
                g_eng.script().set_use_pack_callback((void*)use_pack_process_cb);
            }
            // polaris2 gate P2 (expose-from-script): use-pack push thunk for
            // xi::use(sink).push(pack). Optional symbol — older scripts don't
            // export it and never see the pack push.
            if (g_eng.script().set_use_push_pack_callback) {
                g_eng.script().set_use_push_pack_callback((void*)use_push_pack_cb);
            }
            // Phase 3: trigger access callbacks. Older scripts that don't
            // import xi_script_set_trigger_callbacks just stay null and
            // xi::current_trigger().is_active() returns false.
            // THE CUT: only the trigger INFO callback survives. The image /
            // sources / leader / meta trigger-access callbacks read the deleted
            // TriggerEvent Record members and are gone — pass null for the image
            // and sources slots; the leader and meta wiring is removed entirely.
            if (g_eng.script().set_trigger_callbacks) {
                g_eng.script().set_trigger_callbacks(
                    (void*)trigger_info_cb,
                    nullptr,
                    nullptr);
            }
            // Status callback. Scripts without xi_status.hpp leave this null
            // and xi::status() is a no-op.
            if (g_eng.script().set_status_callback) {
                g_eng.script().set_status_callback((void*)status_cb);
            }
            // C1: image-pool owner get/set thunks. Lets xi::async / xi::parallel_for
            // carry the inspect-thread owner onto worker threads so their pool
            // images stay attributed (instead of anonymous owner=0). Optional
            // symbol — older scripts don't export it and propagation is a no-op.
            if (g_eng.script().set_owner_callbacks) {
                g_eng.script().set_owner_callbacks((void*)owner_get_cb, (void*)owner_set_cb);
            }
            // A4: explicit per-run context thunks. Lets xi::async /
            // xi::parallel_for / xi::spawn_worker carry the run's context (run_id +
            // frame_path + verdict slot + dispatch-thread identity) onto worker
            // threads, so xi::run_id() / xi::current_frame_path() / xi::result() are
            // correct on a worker (the spawn-gap closure) and off-thread reads/pushes
            // fail loud. Replaces the retired trigger-ctx marker + set_run_id /
            // set_run_context setters. Optional symbol — older scripts don't export
            // it and per-run accessors return the 0/"" sentinel with no propagation.
            if (g_eng.script().set_run_ctx_callbacks) {
                g_eng.script().set_run_ctx_callbacks(
                    (void*)run_ctx_get_cb, (void*)run_ctx_set_cb,
                    (void*)run_ctx_run_id_cb, (void*)run_ctx_frame_path_cb,
                    (void*)run_ctx_snapshot_cb, (void*)run_ctx_install_worker_cb,
                    (void*)run_ctx_free_cb);
            }
            // Result callback. Scripts without xi_result.hpp leave this null
            // and xi::result() is a no-op (run_result then defaults to NA).
            if (g_eng.script().set_result_callback) {
                g_eng.script().set_result_callback((void*)result_cb);
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
            if (g_eng.script().set_param) {
                for (auto& [pname, pval] : g_eng.param_cache) {
                    g_eng.script().set_param(pname.c_str(), pval.c_str());
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
            if (g_eng.script().set_instance_def) {
                for (auto& [iname, def] : g_eng.instance_def_cache) {
                    // Replaying a cached def enters the freshly-swapped DLL's plugin
                    // code while we hold g_eng.run_mu + g_eng.script_mu. A plugin that throws
                    // (or faults, via the SEH translator) on an old/incompatible cached
                    // def must NOT terminate the backend mid-swap — log + skip it and
                    // keep replaying the rest (guarded_script_call never touches the
                    // held locks in its catch arms). Best-effort like the param replay:
                    // a nonzero rc (unknown/renamed def) quietly no-ops.
                    guarded_script_call(
                        "replay set_instance_def '" + iname + "'",
                        "replay set_instance_def",
                        [&] { return g_eng.script().set_instance_def(iname.c_str(), def.c_str()); });
                }
                if (!g_eng.instance_def_cache.empty()) {
                    std::fprintf(stderr,
                        "[xinsp2] replayed %zu instance def(s) into reloaded script\n",
                        g_eng.instance_def_cache.size());
                }
            }

            // U2 (docs/new_gen/16): restore the kv channel into the new DLL —
            // see restore_kv_into_new_script_ above.
            restore_kv_into_new_script_(srv);
        }

        // Build log can be large — send as a log message, not inline data.
        if (!res.build_log.empty()) {
            xp::LogMsg lm;
            lm.level = "info";
            lm.msg = res.build_log;
            srv.send_text(lm.to_json());
        }

        // Install the trigger sink so a source's emit_trigger runs an inspect
        // even when NOT continuous (single-shot) — issue/replay works without
        // cmd:start. Needed explicitly here (not just via the guard's dtor):
        // on the FIRST-ever compile no sink existed before the quiesce, so the
        // guard has nothing to restore. Continuous mode (re-armed by the
        // guard's dtor at the prior fps, trigger-only preserved) just adds the
        // free-running timer.
        install_trigger_sink_(&srv);

        // P1-4: the swap succeeded — clear any prior degraded marker. The
        // "@compile" entry's seq/ts_ms double as a running-def generation+recency
        // stamp, so a client can tell a fresh good load from a stale "ok".
        set_status_internal("@compile", "ok");
        // Health contract: the script component is healthy again.
        xi::health().set_script(xi::CompHealth::Ok, "",
                                std::filesystem::path(*src).filename().string());

        // Return success with dll path + diagnostics (warnings only on
        // success; extension still wants them for squiggle).
        std::string data = "{\"dll\":";
        xp::json_escape_into(data, res.dll_path);
        data += ",\"diagnostics\":" + build_diag_json();
        if (reload_guard.was_continuous) data += ",\"resumed_continuous\":true";
        data += "}";
        send_rsp_ok(srv, id, data);
}

/* [cmd_unload_script_ RETIRED at THE CUT (v12) — app-team confirmed, doc 11.
 * Zero in-tree callers; close_project / a fresh compile_and_load clear the
 * live script. The xi::script::unload_script loader primitive stays; only the
 * WS command surface is retired.] */
