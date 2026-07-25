//
// service_cmds.hpp — PRIVATE command / dispatch layer for the service_*.cpp TUs.
//
// Layer 2 of the former service_internal.hpp god-header. It builds on the
// service_state.hpp engine-state foundation (included below) and adds the
// command-handler declarations, the dispatch-pool lifecycle, the script
// host_api / use() / owner / trigger callbacks, the toolchain/project helpers,
// and the plugin-fault policy hooks (note_instance_crash_ / stamp_culprit_ /
// apply_on_fault_policy_ / apply_pending_reinit_).
//
// The fault-boundary TEMPLATES that consume those hooks — guarded_plugin_call /
// guarded_script_call / script_grow_retry — now live in service_guard.hpp (which
// includes this header); a TU pulls that only when it actually guards a plugin /
// script-DLL call. This header therefore no longer pulls xi_seh.hpp; it still
// pulls xi_cabi_adapter.hpp because the apply_*_policy_ hook declarations below
// take a CAbiInstanceAdapter* (and several handler TUs use the type directly).
// TUs that only touch engine state include just service_state.hpp instead.
//
// NOT a public API. Do not include from outside backend/src/service_*.cpp.
//
#pragma once

#include "service_state.hpp"

#include <xi/xi_cabi_adapter.hpp>   // CAbiInstanceAdapter — apply_*_policy_ hook args + direct use in handler TUs

// ---- script host_api + use()/trigger/owner callbacks -----------------------
// These are cast to void* at their (compile_and_load) use site; the EXACT
// signatures must be preserved. Definitions stay in service_main.cpp.
struct CurrentTriggerInfoC {        // mirrors xi::CurrentTriggerInfo (xi_use.hpp)
    xi_trigger_id id;
    int64_t       timestamp_us;
    int32_t       is_active;
    int32_t       _pad;             // align dequeued_at_us to 8 bytes
    int64_t       dequeued_at_us;   // worker-stamped on dequeue from its lane
};
const xi_host_api* script_host_api_();
int  use_exchange_cb(const char* name, const char* cmd, char* rsp, int rsplen);
// polaris2 Gate P2: xi::use(...).process(ScriptPack) → the target's xi.pack@1
// door. 0 ok (*out = new sealed handle the script owns); -1 no such instance;
// -2 door crashed/threw; -3 quarantined; -4 no pack door. (service_sinks.cpp)
int  use_pack_process_cb(const char* name, xi_pack_handle in, xi_pack_handle* out);
// polaris2 gate P2 (expose-from-script): xi::use(sink).push(pack) — deliver a
// script-held sealed pack to a named instance's xi.pack@1 door. Sink targets
// are staged (frame-ordered flush, like use_process_cb); others run inline.
int  use_push_pack_cb(const char* name, xi_pack_handle pack);
uint32_t owner_get_cb();
void     owner_set_cb(uint32_t id);
void     trigger_info_cb(CurrentTriggerInfoC* out);

// ---- toolchain / project helpers -------------------------------------------
void resolve_toolchain_(const std::string& folder);
void read_script_deps_(const std::string& folder,
                       std::vector<std::string>& include_dirs,
                       std::vector<std::string>& link_libs,
                       int& openmp_max_threads,
                       bool& allow_raw_omp);
void set_project_dll_search_(const std::string& folder);
bool apply_process_priority_(const std::string& cls);

// ---- instance crash breadcrumbs --------------------------------------------
void note_instance_crash_(const char* name, const char* why);
void stamp_culprit_(const char* instance, const std::string& plugin);

// ---- item-14 caught-fault policy (defined in service_sinks.cpp) -------------
// Shared by every plugin-entering boundary via guarded_plugin_call (service_guard.hpp).
void apply_on_fault_policy_(const char* name, xi::CAbiInstanceAdapter* adapter);
void apply_pending_reinit_(const char* name, xi::CAbiInstanceAdapter* adapter);

// ---- inspection entry ------------------------------------------------------
void run_one_inspection(xi::ws::Server& srv,
                        int frame_hint = 1,
                        int64_t run_id = 0,
                        const std::string& frame_path = "",
                        int64_t emit_seq = -1,
                        xi::EmitGate* gate = nullptr);   // null = no ordering gate

// ---- dispatch pool lifecycle -----------------------------------------------
void spawn_group_pool_(xi::ws::Server* srv_ptr, int interval_ms);
void stop_group_pool_();
void stop_dispatch_pool_();
void install_trigger_sink_(xi::ws::Server* srv);
void controlled_shutdown_teardown_();
// Project-boundary reset (Wave-2 #3, defined in service_sinks.cpp): drop the
// bus sink + per-source emit-time map AND clear the script replay shadows
// (param_cache / instance_def_cache / persistent kv). One primitive for the
// documented cross-project leak class — previously duplicated verbatim in
// open_project / close_project (and absent from create_project, which also
// replaces the project).
void reset_project_boundary_state_();
#ifdef _WIN32
BOOL WINAPI console_ctrl_handler_(DWORD type);
#endif

// ---- dispatch quiesce guard (used by lifecycle-op cmd handlers) ------------
struct DispatchPoolGuard {
    bool            was_continuous = false;
    int             prior_fps = 10;
    bool            quiesced = false;
    xi::ws::Server* srv = nullptr;
    bool            armed_ = true;
    bool            paused_launches_ = false;
    bool            restore_sink_    = false;

    DispatchPoolGuard() = default;
    DispatchPoolGuard(DispatchPoolGuard&& o) noexcept { *this = std::move(o); }
    DispatchPoolGuard& operator=(DispatchPoolGuard&& o) noexcept {
        was_continuous = o.was_continuous; prior_fps = o.prior_fps;
        quiesced = o.quiesced; srv = o.srv; armed_ = o.armed_;
        paused_launches_ = o.paused_launches_; restore_sink_ = o.restore_sink_;
        o.armed_ = false;
        return *this;
    }
    DispatchPoolGuard(const DispatchPoolGuard&) = delete;
    DispatchPoolGuard& operator=(const DispatchPoolGuard&) = delete;
    ~DispatchPoolGuard() { resume(); }

    // Proof-of-quiesce capability (xi_quiesce_token.hpp): pass `guard.token()`
    // to every destructive PluginManager method. Only this guard (friend) and
    // the explicit QuiesceToken::assert_no_dispatch() escape hatch can mint
    // one — a lifecycle op that forgets the quiesce guard no longer compiles.
    const xi::QuiesceToken& token() const { return token_; }

    void resume() {
        if (!armed_) return;
        armed_ = false;
        // The launch pause is ALWAYS released here — at scope end, never early
        // (skip_resume() deliberately cannot release it; see O2 below).
        if (paused_launches_) { g_eng.inflight.unpause(); paused_launches_ = false; }
        if (restore_sink_ && srv) { install_trigger_sink_(srv); restore_sink_ = false; }
        if (was_continuous && quiesced) {
            bool trig_only = prior_fps <= 0;
            g_eng.continuous_fps = trig_only ? 0 : prior_fps;
            g_eng.continuous = true;
            int interval_ms = trig_only ? 0 : std::max(1, 1000 / std::max(prior_fps, 1));
            spawn_group_pool_(srv, interval_ms);
            std::fprintf(stderr, "[xinsp2] continuous mode resumed\n");
        }
    }
    // skip_resume() (formerly dismiss()): the op ends or replaces the stream
    // (open/close_project) or must leave dispatch stopped (partial
    // commit_group) — so on destruction do NOT respawn continuous mode and do
    // NOT re-install the bus sink. Unlike the old dismiss(), it does NOT
    // release the launch pause: that is ALWAYS done in the destructor
    // (resume()), never early. Releasing the pause before scope end let a
    // straggler source-emit one-shot (already past the sink read) launch into
    // a just-unmapped DLL mid-teardown — the O2 use-after-unload class. With
    // this split that bug is unwritable: the pause outlives every statement
    // in the guarded scope by construction.
    void skip_resume() {
        was_continuous = false;   // don't respawn continuous mode at scope end
        restore_sink_  = false;   // don't re-install the bus sink either
    }

private:
    // Minted via friendship (private ctor). Lives exactly as long as the
    // quiesce window this guard represents.
    xi::QuiesceToken token_;
};

DispatchPoolGuard quiesce_dispatch_for_lifecycle_op_(const char* op_name, xi::ws::Server* srv);

// ---- command handlers (dispatch table entries; defined across service_cmd_*.cpp) ----
void cmd_ping_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_version_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_shutdown_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_compile_and_load_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_save_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_commit_working_copy_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_discard_working_copy_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_load_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_create_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_open_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_close_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_timer_fps_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_run_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_start_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_stop_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_exchange_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_prepare_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_commit_group_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_crash_reports_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_clear_crash_reports_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_watchdog_ms_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_graph_capture_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_graph_snapshot_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_get_state_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_recent_errors_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_status_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_image_pool_stats_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_dispatch_stats_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_metrics_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_open_project_warnings_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_list_params_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_param_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_list_instances_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_instance_def_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_get_instance_def_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_create_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_remove_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_rename_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_save_instance_config_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_get_dashboard_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_process_priority_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_list_plugins_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_rescan_plugins_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_export_project_plugin_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_recompile_project_plugin_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_rebuild_plugins_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_get_plugin_ui_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_toolchain_health_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_toolchain_override_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_get_health_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
