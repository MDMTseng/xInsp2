//
// service_guard.hpp — the fault-boundary templates shared by every backend TU
// that enters plugin or script-DLL code: guarded_plugin_call (the ONE plugin-entry
// SEH/exception boundary with the item-14 quarantine/on-fault ritual),
// guarded_script_call (its smaller script-DLL sibling used at the compile_and_load
// swap-time replay sites), and the script_grow_retry buffer-protocol helper. Split
// out of service_cmds.hpp so only the TUs that actually guard a call pull xi_seh.hpp.
//
// Includes service_cmds.hpp for the plugin-fault policy HOOKS these templates call
// (note_instance_crash_ / stamp_culprit_ / apply_on_fault_policy_ /
// apply_pending_reinit_), the CAbiInstanceAdapter definition, and the shared engine
// state. Include this ONLY from the .cpp TUs that use a guarded_* / script_grow_retry
// template; every other service_*.cpp includes just service_cmds.hpp (no xi_seh).
//
// NOT a public API. Do not include from outside backend/src/service_*.cpp.
//
#pragma once

#include "service_cmds.hpp"

#include <xi/xi_seh.hpp>   // seh_exception + recover_seh_stack_or_die (used inline by the templates below)

using xi::seh_exception;

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

