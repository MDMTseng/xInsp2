//
// service_cmds.hpp — PRIVATE command / dispatch layer for the service_*.cpp TUs.
//
// Layer 2 of the former service_internal.hpp god-header. It builds on the
// service_state.hpp engine-state foundation (included below) and adds the
// command-handler declarations, the dispatch-pool lifecycle, the script
// host_api / use() / owner / trigger callbacks, the toolchain/project helpers,
// and the two fault-boundary templates (guarded_plugin_call / guarded_script_call)
// with the plugin-fault policy hooks they call.
//
// It pulls xi_seh.hpp (seh_exception + recover_seh_stack_or_die used inline by
// the templates below) and xi_cabi_adapter.hpp (guarded_plugin_call dereferences
// CAbiInstanceAdapter). TUs that only touch engine state and never the command /
// guarded / pool surface include just service_state.hpp instead of this header.
//
// NOT a public API. Do not include from outside backend/src/service_*.cpp.
//
#pragma once

#include "service_state.hpp"

#include <xi/xi_seh.hpp>            // seh_exception + recover_seh_stack_or_die (guarded_* templates)
#include <xi/xi_cabi_adapter.hpp>   // CAbiInstanceAdapter — dereferenced by guarded_plugin_call

using xi::seh_exception;

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
// Shared by every plugin-entering boundary via guarded_plugin_call below.
void apply_on_fault_policy_(const char* name, xi::CAbiInstanceAdapter* adapter);
void apply_pending_reinit_(const char* name, xi::CAbiInstanceAdapter* adapter);

// ---- guarded_plugin_call: the ONE plugin-entry fault boundary ----------------
// Root cause (Wave-2 #1): the item-14 six-step ritual — quarantined? gate →
// pending-reinit apply → re-check → stamp_culprit_ → try{enter plugin} →
// catch(seh){note_instance_crash_ + apply_on_fault_policy_ +
// recover_seh_stack_or_die} / catch(std){note + policy} — was HAND-COPIED at
// every plugin-entering site and drifted. Worst drift: cmd_prepare_instance_ /
// cmd_commit_group_ caught seh_exception only via `catch (const std::exception&)`
// (seh_exception derives from it), so recover_seh_stack_or_die never ran — after
// a plugin STACK_OVERFLOW the WS thread's stack guard page stayed CONSUMED and
// the next deep call on that thread corrupted memory instead of faulting; those
// sites also skipped the quarantine gate and all crash bookkeeping, so a
// prepare()/commit()-only crash-loop never tripped health/reinit/refuse. This
// helper owns the whole ritual; real per-site differences are explicit
// parameters, not divergent copies.
//
// `gate_quarantined`: whether a quarantined instance is REFUSED without entering
// plugin code. TRUE for the data/exchange plane (process/pack door/exchange —
// exactly what item-14 quarantine exists to stop). FALSE for the config-plane
// re-enable surface: set_def / commit are the DOCUMENTED operator un-quarantine
// path (their success path runs set_inst_state(name, Active), which lifts the
// gate — see set_inst_state in service_dispatch.cpp); gating them would make an
// on_fault=refuse quarantine unrecoverable through its own documented remedy.
// get_def is also ungated: reading the faulted config is how an operator repairs
// it. prepare() is ALSO ungated (round-3 S1): it was briefly gated on the theory
// that its success never sets Active so it "can't lift a quarantine", but for a
// STAGED plugin the documented on_fault=refuse remedy is prepare_instance →
// commit_group — gating prepare dead-ended that recovery at step 1. prepare is
// config-plane staging, exactly the surface quarantine must leave open.
//
// The on-fault POLICY (apply_on_fault_policy_) runs unconditionally on a caught
// crash/throw when the instance is a C-ABI adapter: every previously-complete
// site (use_push_pack_inline_ / use_exchange_cb / use_pack_process_cb /
// cmd_exchange_instance_) applied it in both catch arms, so there is no
// per-site fork to preserve. A null `adapter` (non-C-ABI instance) skips the
// gates and policy but keeps the culprit stamp + catch + stack recovery.
struct PluginCallResult {
    enum class Kind { Ok, Quarantined, Crashed, Threw };
    Kind        kind = Kind::Ok;
    unsigned    seh_code = 0;   // Kind::Crashed only
    std::string what;           // e.what() text (Crashed / Threw; "non-std exception" for catch(...))
    bool ok() const { return kind == Kind::Ok; }
};

template <class Fn>
PluginCallResult guarded_plugin_call(const char* name,
                                     xi::CAbiInstanceAdapter* adapter,
                                     const std::string& plugin,
                                     const char* what,           // entered surface, e.g. "exchange()" / "pack door"
                                     bool gate_quarantined,
                                     Fn&& fn) {
    using K = PluginCallResult::Kind;
    PluginCallResult r;
    if (adapter) {
        if (gate_quarantined && adapter->quarantined()) { r.kind = K::Quarantined; return r; }
        if (adapter->reinit_pending()) {
            apply_pending_reinit_(name, adapter);
            if (gate_quarantined && adapter->quarantined()) { r.kind = K::Quarantined; return r; }
        }
    }
    stamp_culprit_(name, plugin);
    try {
        fn();
        return r;
    } catch (const seh_exception& e) {
        std::fprintf(stderr, "[xinsp2] %s '%s' crashed: 0x%08X (%s)\n",
                     what, name, e.code, e.what());
        char why[96]; std::snprintf(why, sizeof(why), "%s crashed: 0x%08X", what, e.code);
        note_instance_crash_(name, why);
        if (adapter) apply_on_fault_policy_(name, adapter);
        // Swallowed on a surviving thread — restore the stack guard page after an
        // overflow (or hard-exit for respawn) BEFORE returning toward deep code.
        xi::recover_seh_stack_or_die(e.code, what);
        r.kind = K::Crashed; r.seh_code = e.code; r.what = e.what();
        return r;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[xinsp2] %s '%s' threw: %s\n", what, name, e.what());
        char why[96]; std::snprintf(why, sizeof(why), "%s threw an exception", what);
        note_instance_crash_(name, why);
        if (adapter) apply_on_fault_policy_(name, adapter);
        r.kind = K::Threw; r.what = e.what();
        return r;
    } catch (...) {
        std::fprintf(stderr, "[xinsp2] %s '%s' threw a non-std exception\n", what, name);
        char why[96]; std::snprintf(why, sizeof(why), "%s threw an exception", what);
        note_instance_crash_(name, why);
        if (adapter) apply_on_fault_policy_(name, adapter);
        r.kind = K::Threw;
        // Round-3 S5: leave a real message — converted call sites append r.what
        // to user-facing errors ("xxx error: " + r.what) and an empty tail read
        // as a truncated reply.
        r.what = "non-std exception";
        return r;
    }
}

// ---- guarded_script_call: the script-DLL sibling of guarded_plugin_call -----
// Root cause (round-3 W2 #6): the compile_and_load swap-time replay sites
// (set_instance_def replay, kv migrate-restore, kv plain-restore) hand-copied
// the same try{enter script DLL}catch(seh){stderr + recover_seh_stack_or_die}
// catch(std){stderr} ritual three times, with the crash/threw log lines one
// edit away from drifting. A script-DLL call has NO adapter, quarantine gate,
// or on-fault policy — guarded_plugin_call's item-14 machinery genuinely does
// not apply — so this is a deliberately smaller helper beside it, not a
// parametrization of it.
//   `label`       — the human stderr identity; may carry per-item detail
//                   (e.g. "replay set_instance_def 'blobs'").
//   `recover_ctx` — the recover_seh_stack_or_die context (the site class,
//                   without per-item detail — matches the pre-helper strings).
//   `fn`          — returns the thunk's int rc; nonzero maps to Refused. A
//                   caller that ignores the rc (set_instance_def's
//                   best-effort replay) just treats Refused like Ok.
// Deliberately NO catch(...): the blocks this replaces let a non-std throw
// propagate to the dispatch shell's top-level guard, and that stays true.
enum class ScriptCallOutcome { Ok, Refused, Crashed, Threw };

template <class Fn>
inline ScriptCallOutcome guarded_script_call(const std::string& label,
                                             const char* recover_ctx, Fn&& fn) {
    try {
        return fn() == 0 ? ScriptCallOutcome::Ok : ScriptCallOutcome::Refused;
    } catch (const seh_exception& e) {
        std::fprintf(stderr, "[xinsp2] %s crashed: 0x%08X (%s) — skipped\n",
                     label.c_str(), e.code, e.what());
        // Swallowed on a surviving thread — restore the stack guard page after
        // an overflow (or hard-exit for respawn) BEFORE returning toward deep code.
        xi::recover_seh_stack_or_die(e.code, recover_ctx);
        return ScriptCallOutcome::Crashed;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[xinsp2] %s threw: %s — skipped\n",
                     label.c_str(), e.what());
        return ScriptCallOutcome::Threw;
    }
}

// ---- script thunk grow-retry (Wave-2 #4) -------------------------------------
// Root cause: the script DLL buffer protocol (`n = fn(buf, len); n < 0 ⇒ grow to
// -n and retry`) was hand-rolled ~9x across the cmd handlers, and the copies
// FORKED on what -1 means. The contract (xi_script_support.hpp / xi_kv.hpp):
//   * xi_script_exchange_instance / xi_script_get_instance_def return -1 for
//     "instance not found" (terminal) and -needed for a too-small buffer. A
//     GENUINE -needed of -1 cannot reach us: needed==1 only overflows when
//     buflen < 2, and every caller starts with a KiB-scale buffer (and the
//     retry buffer is always >= 1024+). So for those thunks -1 is TERMINAL —
//     pass minus_one_is_terminal = true (retrying it was a harmless but wrong
//     extra call in most old copies; cmd_get_instance_def_ had the correct fork).
//   * xi_script_list_params / list_instances / kv_get have NO -1 error return —
//     every negative is -needed — pass minus_one_is_terminal = false.
// The resize widens through int64 first: -(int64_t)n, because -INT_MIN is UB.
template <class Buf, class Fn>
inline int script_grow_retry(Buf& buf, bool minus_one_is_terminal, Fn&& fn) {
    int n = fn(buf.data(), (int)buf.size());
    if (n < 0 && !(minus_one_is_terminal && n == -1)) {
        buf.resize((size_t)(-(int64_t)n) + 1024);
        n = fn(buf.data(), (int)buf.size());
    }
    return n;
}

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
