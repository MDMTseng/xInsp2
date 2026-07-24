//
// service_sinks.cpp — the script host_api callback surface: pipeline graph
// capture glue, ordered output sinks (stage/drain/flush), trigger-loop state,
// trigger-access script callbacks, image-pool owner thunks, watchdog-slot
// arm/disarm, and script_host_api_. Split from service_main.cpp
// (behavior-preserving; see service_state.hpp / service_cmds.hpp).
//
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <xi/xi_pack_abi.hpp>   // PackRegistry retain/release (use_push_pack_cb)
#include <xi/xi_pack_contract.hpp> // U1 fault short-circuit (use_pack_process_cb, doc 15)

#include "service_cmds.hpp"
#include <xi/xi_use.hpp>

// ---- Pipeline graph capture (stage 2) ----------------------------------
// Opt-in dataflow-graph capture lives in xi_graph_capture.hpp (xi::GraphCapture
// singleton). OFF by default → the hot path below pays only a relaxed atomic
// load. The ws handlers (graph_capture / graph_snapshot) drive it.

// Defined after g_eng.plugin_mgr (declared further down); records a per-instance
// process() crash so a crash loop is visible via get_state.
// note_instance_crash_ declared in service_cmds.hpp.

// Part III G2.1 — stamp the process-global crash culprit (xi::crash::g_culprit)
// with the instance/plugin the host is about to enter, plus that plugin's
// folder + dll so the FE can quarantine it on a death. Defined after
// g_eng.plugin_mgr. Cheap on the dispatch hot path: a per-thread cache means the
// manager lock is taken only when the active plugin on this thread changes.
// stamp_culprit_ declared in service_cmds.hpp.

// ---- item 14: post-fault quarantine policy (adoption-map item 14) -----------
// The health-overlay + escalation POLICY lives HERE, at the fault boundary; the
// CAbiInstanceAdapter carries the mechanical per-instance state (the policy value,
// the quarantine flag, the in-place rebuild) because its per-instance CallScope
// gate is the natural serialization point. See xi_fault_policy.hpp +
// docs/new_gen/04-health-contract.md (§ "Quarantine policy").

// on_fault=refuse: pull the instance out of service. Sets the fail-fast gate,
// marks it failed/quarantined in the health contract, and surfaces ONE operator-
// visible error. Idempotent — a per-frame refuse never re-emits (the gate + the
// health overlay both coalesce), so it can't spam.
static void quarantine_instance_(const char* name, xi::CAbiInstanceAdapter* adapter) {
    if (adapter->quarantined()) return;
    adapter->set_quarantined(true);
    xi::health().mark_instance_fault(name, xi::CompHealth::Failed, xi::kReasonQuarantined);
    push_recent_error(name,
        "instance quarantined (on_fault=refuse) — pulled from service after a caught "
        "process() fault; re-enable by re-committing its config (set_instance_def / "
        "commit_group)");
}

// Consult the caught-fault policy AFTER note_instance_crash_ has already marked the
// instance runtime-`degraded`. reuse: nothing more (stays in service). reinit:
// request a rebuild before the instance's next use. refuse: quarantine now.
// External linkage (not static): cmd_exchange_instance_ (service_cmd_dispatch.cpp)
// shares this fault boundary for its own plugin-entering exchange() call.
void apply_on_fault_policy_(const char* name, xi::CAbiInstanceAdapter* adapter) {
    switch (adapter->on_fault()) {
        case xi::OnFault::Reuse:  break;
        case xi::OnFault::Reinit: adapter->request_reinit(); break;
        case xi::OnFault::Refuse: quarantine_instance_(name, adapter); break;
    }
}

// Perform a requested (on_fault=reinit) rebuild before the next process(). A clean
// rebuild clears the runtime-fault overlay (the instance is healthy again); a
// failed rebuild keeps the old instance live and escalates to refuse after
// kReinitEscalateAfter consecutive failures. Runs on the caller thread just before
// process(); reinit() serializes itself via the instance's CallScope.
// External linkage (not static): shared with cmd_exchange_instance_ — see
// apply_on_fault_policy_ above.
void apply_pending_reinit_(const char* name, xi::CAbiInstanceAdapter* adapter) {
    // H5: CONSUME the pending flag with one atomic test-and-clear before rebuilding.
    // Two callers can both observe reinit_pending()==true and reach here (the outer
    // check is a cheap fast-path, not a gate); the exchange lets exactly ONE win.
    // A loser returns without touching reinit()/escalation, so one fault → exactly
    // ONE rebuild and the escalation counter is owned by a single thread per fault
    // episode (no note_reinit_fail crossing the quarantine threshold ahead of a
    // concurrent reset_reinit_fails). The winner runs the whole rebuild + accounting
    // sequence below, exactly as the single-fault case always did.
    if (!adapter->consume_reinit_pending()) return;
    if (adapter->reinit()) {
        adapter->reset_reinit_fails();
        xi::health().clear_instance_degraded(name);   // recovered → ok / running
    } else if (adapter->note_reinit_fail() >= xi::kReinitEscalateAfter) {
        quarantine_instance_(name, adapter);
    }
}

// ---- ordered output sinks: stage during inspect, flush in frame order -----------
// A script's xi::use(<sink>).process(rec) must NOT run inline under parallel dispatch
// — the sink's side effect (comm → PLC, expose push) would land in COMPLETION order,
// not frame order. Instead each call is STAGED on the running worker thread and
// flushed AFTER the inspect, inside the EmitTurn gate (run_one_inspection), in call
// order — exactly like this run's vars/result emit. Reuses the same resource-
// ownership discipline as a bus TriggerEvent (release_trigger_event_). thread_local
// so parallel workers stage independently. (use() is script-only — a plugin can't
// re-enter it — so staging only ever happens inside an inspect, where the guard +
// flush bracket g_staged.)
// StagedEmit struct moved to service_state.hpp; g_staged DEFINED here.
thread_local std::vector<StagedEmit> g_staged;

// ---- polaris2 gate P2: expose-from-script (use(sink).push(pack)) ----------------
// Deliver a SEALED pack to `name`'s xi.pack@1 door NOW, on this thread. The pack
// handle is BORROWED (caller keeps its ref; run_pack_door borrows too) and crosses
// AS-IS — no re-encode, no $seq stamping (a sealed pack is immutable, and identity
// with a direct host-side dump is the contract; $channel/$seq ride as pack entries).
// The ack pack the door returns is dropped (fire-and-forget, mirroring how the
// staged Record flush drops the sink's reply). Same fault discipline as
// use_process_inline_: quarantine gate, pending reinit, culprit stamp, SEH boundary
// (a crashed door -> -2, on_fault policy applied; the borrowed input handle is
// still ours, so nothing leaks). Return codes: 0 delivered; -1 no such instance;
// -2 door crashed; -3 quarantined; -4 no xi.pack@1 door.
static int use_push_pack_inline_(const char* name, xi_pack_handle pack) {
    auto inst = xi::InstanceRegistry::instance().find(name);
    if (!inst) return -1;
    auto* adapter = dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get());
    if (!adapter || !adapter->has_pack_door()) return -4;
    // Wave-2 #1: the six-step ritual now lives in guarded_plugin_call (data
    // plane ⇒ quarantine-gated). The ack pack is dropped (fire-and-forget).
    auto r = guarded_plugin_call(name, adapter, inst->plugin_name(), "pack door",
                                 /*gate_quarantined=*/true, [&] {
        xi_pack_handle ack = adapter->run_pack_door(pack);
        if (ack != XI_PACK_NULL) xi::PackRegistry::instance().release(ack);
    });
    if (r.kind == PluginCallResult::Kind::Quarantined) return -3;
    if (!r.ok()) return -2;
    return 0;
}

// J4: reject a use(sink).push() issued off the dispatch thread. Defined below,
// next to the READ-path guard (warn_trigger_off_thread_) whose A4 RunContext
// (g_run_ctx owner_tid) they reuse — forward-declared here so use_push_pack_cb
// can call them before those context helpers are defined.
static bool push_off_dispatch_thread_();
static void warn_push_off_thread_(const char* name);

// xi::use().push(pack) entry wired into the script DLL (optional symbol —
// xi_script_set_use_pack_callback). A declared ORDERED SINK target is staged and
// flushed after the inspect in frame order: StagedEmit.rec is a TriggerEvent, whose `pack` slot carries our
// RETAINED ref (release_trigger_event_ / drain_staged_emits_ already release it
// on every flush/drop path — the dual-carry discipline, reused). A non-sink
// pack-door target runs inline. Fail-fast at call time (missing instance / no
// door) so the script's push() gets an honest false instead of a silent
// flush-time drop.
int use_push_pack_cb(const char* name, xi_pack_handle pack) {
    if (!name || pack == XI_PACK_NULL) return -1;
    // J4: push() STAGES into g_staged, which is drained ONLY on the dispatch thread
    // that runs the inspect (drain_/flush_staged_emits_). A push from a xi::parallel_for
    // / xi::async CHILD worker would stage into THAT child's thread_local g_staged,
    // which is never flushed → the retained pack ref leaks (→ exhaustion under OpenMP
    // pool reuse) and the delivery is silently dropped. push() is valid ONLY on the
    // trigger/dispatch thread; off-thread is a fail-loud programming error (the WRITE
    // analogue of the READ guard on current_trigger()). Reject BEFORE retaining, loudly
    // + once per sink name.
    if (push_off_dispatch_thread_()) { warn_push_off_thread_(name); return -6; }
    auto inst = xi::InstanceRegistry::instance().find(name);
    if (!inst) return -1;
    auto* a = dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get());
    if (!a || !a->has_pack_door()) return -4;
    if (a->is_sink()) {
        // Our staged ref: the script's own ScriptPack ref may die right after
        // push() returns, so retain BEFORE returning. Balanced by
        // release_trigger_event_ at flush/drain.
        //
        // L2 (historical): under the old counted ledger this HAD to be
        // retain_untagged to avoid stranding a phantom script-owner bucket.
        // Under the single-creator-tag registry every retain is an untracked
        // ++rc, so this pairs trivially with the bus releaser
        // (release_trigger_event_ → f_release_for_bus → release_as(pack, 0)).
        xi::PackRegistry::instance().retain_untagged(pack);
        StagedEmit item;
        item.target   = name;
        item.rec.pack = pack;
        g_staged.push_back(std::move(item));
        return 0;
    }
    return use_push_pack_inline_(name, pack);
}

// Script→plugin request/reply (xi::use(name).exchange(cmd)). exchange() ENTERS
// PLUGIN CODE, so it carries the same item-14 fault surface as its siblings
// (use_push_pack_inline_ / use_pack_process_cb): refuse → -3 without entering
// plugin code, pending reinit applied first, culprit stamped, and a caught
// crash feeds note_instance_crash_ + apply_on_fault_policy_ — otherwise a
// quarantined instance is still entered every frame via exchange(), and an
// exchange()-only crash-loop never trips health/reinit/refuse.
// NOTE on the crash return: this surface keeps -1 (NOT the pack path's -2).
// The script-side caller (UseProxy::exchange, xi_use.hpp) treats -1 as the
// terminal miss and RETRIES any other negative as "buffer too small, need -n
// bytes" — a -2 would re-enter a just-crashed plugin once more. The -3
// quarantine refuse is safe under that retry: the retry hits this gate again
// and is refused without touching the plugin.
int use_exchange_cb(const char* name, const char* cmd,
                           char* rsp, int rsplen) {
    if (!name) return -1;
    auto inst = xi::InstanceRegistry::instance().find(name);
    if (!inst) return -1;
    // Item-14 gates live on the C-ABI adapter; a non-adapter instance has no
    // quarantine/reinit state — guarded_plugin_call skips the gates/policy for
    // a null adapter but keeps the culprit stamp + catch + stack recovery.
    auto* adapter = dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get());
    int n = 0;
    auto r = guarded_plugin_call(name, adapter, inst->plugin_name(), "exchange()",
                                 /*gate_quarantined=*/true, [&] {
        std::string result = inst->exchange(cmd);
        int need = (int)result.size();
        if (rsplen < need + 1) { n = -need; return; }
        std::memcpy(rsp, result.data(), result.size());
        rsp[result.size()] = 0;
        n = need;
    });
    if (r.kind == PluginCallResult::Kind::Quarantined) return -3;
    if (!r.ok()) return -1;   // crash/throw stays -1 — see the retry note above
    return n;
}

// polaris2 Gate P2 — xi::use(...).process(ScriptPack) wired into the script DLL:
// drive the target plugin's xi.pack@1 pack door with a sealed host pack. Runs
// INLINE on this thread under the same item-14 fault gates as the Record path
// (use_process_inline_): refuse → -3 without entering plugin code, pending
// reinit applied first, culprit stamped, SEH/throw caught → -2 with the same
// crash bookkeeping + on-fault policy. `in` is borrowed (script keeps its ref);
// on 0 `*out` is a NEW sealed handle the SCRIPT owns (or XI_PACK_NULL if the
// door hard-failed). No pack analogue of the DocRegistry ref accounting is
// needed: pack lifetime is pure handle refcount (PackRegistry), nothing is
// reserved for an adopter up front.
//
// U3 (docs/new_gen/17): the v0 "sink target runs inline" gap is CLOSED by
// doctrine, not by staging — a process() call on a declared ORDERED SINK is
// rejected at call time (-5, fail-loud). process() is the request-reply
// surface; a staged call's reply cannot exist until the post-inspect flush,
// so staging here would force an empty return indistinguishable from the
// documented "door hard failure" empty — a silent semantic fork on the
// target's declared role. The sink feed is use(sink).push(pack) (staged +
// flushed in frame order, use_push_pack_cb above). The script side maps -5 to
// an empty pack + a once-per-name error log naming push().
int use_pack_process_cb(const char* name, xi_pack_handle in, xi_pack_handle* out) {
    if (out) *out = XI_PACK_NULL;
    if (!name || !out) return -1;
    // U1 fault SHORT-CIRCUIT (docs/new_gen/15): a poison input never enters
    // plugin code. The funnel mints a NEW sealed fault pack — the original
    // "$fault"/"$fault_key"/"$fault_detail" (+ "$seq") with this hop stamped
    // as "$src" and appended to the "$prov" chain — and returns it as the
    // call's result (rc 0). This is the pack mirror of the Record path's
    // `if (input.is_na()) return Record::na(reason).set_src(name)`
    // (UseProxy::process, xi_use.hpp), and like it, it runs BEFORE the
    // instance lookup: poison propagates even through a typo'd, quarantined
    // or door-less name — the frame's failure is already explained by the
    // carried reason, and the plugin must not run either way.
    if (xi::pack_contract::is_fault(xi::pack_v1_iface(), in)) {
        *out = xi::pack_contract::propagate_fault(xi::pack_v1_iface(), in, name);
        return 0;
    }
    auto inst = xi::InstanceRegistry::instance().find(name);
    if (!inst) return -1;
    auto* adapter = dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get());
    if (!adapter) return -4;                       // non-C-ABI instance: no door
    // U3: static misuse — checked BEFORE the fault gates (the plugin is never
    // entered, no health/quarantine state is touched).
    if (adapter->is_sink()) return -5;
    // Round-3 S4: the Wave-2 consolidation moved the -4 no-pack-door check
    // BEFORE the quarantine gate (which lives inside guarded_plugin_call), so a
    // quarantined door-less instance started answering -4 where the old
    // contract said -3 — scripts branching on -3 ("refused, operator action
    // needed") vs -4 ("wrong target, fix the script") saw the wrong bucket.
    // Restore the old precedence: gates first, door check after. The pre-gate
    // duplicates guarded_plugin_call's check on purpose (it must run before the
    // -4 check, which in turn must run before the plugin is entered); the
    // re-check inside guarded_plugin_call is a harmless no-op.
    if (adapter->quarantined()) return -3;
    if (adapter->reinit_pending()) {
        apply_pending_reinit_(name, adapter);
        if (adapter->quarantined()) return -3;
    }
    if (!adapter->has_pack_door()) return -4;      // Record-only plugin
    // Wave-2 #1: shared fault boundary (data plane ⇒ quarantine-gated).
    auto r = guarded_plugin_call(name, adapter, inst->plugin_name(), "pack door",
                                 /*gate_quarantined=*/true, [&] {
        *out = adapter->run_pack_door(in);         // OwnerGuard + CallScope inside
    });
    if (r.kind == PluginCallResult::Kind::Quarantined) return -3;
    if (!r.ok()) {
        *out = XI_PACK_NULL;                       // a torn result is never handed out
        return -2;
    }
    return 0;
}

// ---- Trigger loop state ----
//
// CONTINUOUS RUN HAS TWO DRIVERS — don't confuse them:
//
//   1. TRIGGERS (the real driver). Image sources emit_trigger() → the bus/lanes
//      run inspect() per frame. This is the production path: a source (camera,
//      PLC-trigger bridge, or any self-emitting plugin like burst_source /
//      frame_source / local_image_source) decides WHEN an inspection happens. A
//      run without a source/trigger is, semantically, meaningless.
//
//   2. The SYNTHETIC TIMER TICK (a convenience). A timer thread injects an EMPTY
//      trigger every g_eng.timer_interval_ms so a SOURCE-LESS script still ticks (the
//      dev edit→run loop, the no-camera HMI demo). It's NOT a real inspection
//      driver — there's no image/trigger behind it; xi::current_trigger() is
//      inactive. Set it to 0 (cmd:start {fps:0} / --autostart-fps=-1 /
//      runtime.timer_fps:0) for TRIGGER-ONLY, which is what any source-driven
//      project should use. If you need periodic runs WITH meaning, write a source
//      plugin that emits on a timer (that's the supported way) rather than relying
//      on this empty tick.
//
// Kept because it's harmless and handy for source-less bring-up; just remember
// when reading/writing tests: fps>0 = "tick a source-less script", fps<=0 =
// "sources are the only driver".
//
// When running in continuous mode (cmd: start), a worker thread waits for
// trigger signals from image sources and calls inspect() for each frame.
// FPS the most recent cmd:start was launched with. compile_and_load
// captures this to re-arm continuous mode at the same rate after the
// reload completes — without it, mid-run hot-reload would silently
// halt the stream.
// Live timer-tick interval (ms). The continuous timer thread reads this every
// loop, so the synthetic-tick rate can be retuned WHILE RUNNING (cmd:set_timer_fps)
// — 0 = trigger-only (no ticks). Seeded from cmd:start's fps / project.json
// runtime.timer_fps. Default 100 (10fps) matches the historical default.
// Reserve stack headroom so the crash filter can dump after a script
// STACK_OVERFLOW; called at the top of each inspect-running thread. Forwards to
// the extracted forensics leaf (xi_crash_dump.hpp).
void reserve_fault_stack() { xi::crash::reserve_fault_stack(); }
// Synthetic-tick timer thread (`g_eng.timer_thread`): pushes an empty event at the
// configured fps so scripts without a trigger source still get periodic
// dispatch. The worker threads themselves are per-lane (see GroupLane).
// Result ordering (parallelism.result_order / per-group result_order). When
// ordered, each popped event gets a gapless emit sequence (assigned at dequeue
// under the lane lock so it follows arrival order) and an EmitTurn gate makes
// workers emit run_result/vars/run_finished in that order. Compute still runs
// fully parallel; only emission is serialized. In "completion" mode emit_seq is
// -1 (emit immediately).
//
// EmitGate (per-lane cursor) + EmitTurn (the ordered-emit RAII gate) live in
// xi_emit_gate.hpp so the primitive is unit-testable. EmitTurn is claimed at the top
// of run_one_inspection, wait_turn()'d before the emit, complete()'d after, and its
// dtor backstops any early-return — see that header. We pass &g_eng.continuous as the
// "keep going" flag so a stop unblocks a waiter.
using xi::EmitGate;
using xi::EmitTurn;
// Serialise cmd:run dispatch threads so vars arrive in run_id
// order. Threads queue up here and the watchdog operates on whichever
// one is currently inside run_one_inspection — only one at a time.

// Structural owner for the detached cmd:run / one-shot inspect threads — owns the
// bump-before-detach + bail-if-shutting-down + drain-on-teardown protocol that was
// the shutdown-window UAF class when hand-copied at every site. Defined + unit-
// tested in xi_inflight_runs.hpp.

// Crash breadcrumb model + minidump machinery moved to xi_crash_dump.hpp
// (xi::crash::). These thin forwarders keep the dispatch hot-path call sites
// (crash_ctx()/crash_set()/crash_set_phase()) terse and unchanged.
xi::crash::Context& crash_ctx() { return xi::crash::ctx(); }
// crash_set / crash_set_phase are defined inline in service_state.hpp.

// Watchdog (P2.4). When > 0, inspect() calls have this many ms of wall-
// clock budget. Default 0 = disabled (back-compat). Set via
// cmd:set_watchdog_ms or --watchdog=N.
//
// Per-worker deadlines: the parallel dispatch pool (parallelism.dispatch_threads
// > 1) runs N inspects at once, so the watchdog tracks a SLOT per in-flight
// inspect (each arms a free slot on entry, clears it on exit). The monitor scans
// all slots. On a deadline breach the overrunning inspect gets a grace window
// (a merely-slow frame finishes and is left alone); if the SAME inspect is
// still overrun after the grace it is wedged, and the process is unrecoverable
// (a forced thread kill would leak the per-instance lock + risk heap
// corruption), so the backend exits and the FE supervisor respawns a clean one.
// See docs/guides/writing-a-script.md (Parallel dispatch) + internals/fe-be.md.
// Per-slot inspect deadline (steady_clock epoch-ms); 0 = free. Written by the
// dispatch/run thread that owns the slot, read by the watchdog thread.
// WATCHDOG_EXIT_CODE moved to service_state.hpp.

// Claim a free watchdog slot for `deadline` (steady-clock epoch-ms). Returns the
// slot index, or -1 if all slots are busy (then this inspect runs unwatched —
// only possible with >64 concurrent inspects, far beyond any real pool).
int wd_arm(int64_t deadline) {   // decl in header
    for (int i = 0; i < WD_SLOTS; ++i) {
        int64_t expect = 0;
        if (g_eng.wd_deadlines[i].compare_exchange_strong(expect, deadline)) return i;
    }
    return -1;
}
void wd_disarm(int slot) { if (slot >= 0) g_eng.wd_deadlines[slot].store(0); }   // decl in header
// (The watchdog loop scans the slots inline now — it needs the per-slot deadline
// values to snapshot which inspects it targeted, so a fresh frame that overruns
// during the grace isn't mistaken for the originally-stuck one. See the monitor
// thread below.)

// Server pointer for emits that happen off the serving thread (status_cb, the
// dropped-frame markers). Atomic so a worker/plugin thread loads it once and the
// shutdown null-out can't tear a read. A pointer load is a plain mov on x86-64 —
// no hot-path cost.

// ---- Trigger access (script callbacks) ---------------------------------
// Set by the worker thread (or run_one_inspection) before invoking the
// script. The script reads via xi::current_trigger() through the three
// trigger_*_cb functions below. thread_local so multiple parallel
// dispatch threads can each have their own current trigger.
thread_local const xi::TriggerEvent* g_current_trigger = nullptr;   // DEFINED here (decl in header)

// F4: invoked from the trigger info thunk's "no current trigger" branch. The
// relational detection now rides the A4 explicit RunContext (g_run_ctx) instead
// of a separate integer marker: the context is installed on the dispatch thread
// (carrying owner_tid + had_trigger) and PROPAGATED BY VALUE onto xi::async /
// xi::parallel_for / xi::spawn_worker workers, while g_current_trigger stays
// thread_local and is NOT inherited. So the ambient current_trigger() is null on
// a worker even though the context is present. Fire ONLY when this is a worker of
// a TRIGGERED run — the context is present, its owner_tid differs from this
// thread (⇒ a spawned worker, not the dispatch thread), and the run carried a
// trigger (had_trigger). A dispatch thread of a triggered frame never reaches
// here (g_current_trigger != null); a timer-tick / plain cmd:run thread has
// had_trigger == false, so reading current_trigger() there is the legitimate "no
// trigger" case → quiet. A hand-rolled `#pragma omp` region (NOT the blessed
// primitives) keeps the context of the thread it runs on, so an off-thread read
// from one is not flagged — the same documented trade as before.
static void warn_trigger_off_thread_() {
    if (!(g_run_ctx && g_run_ctx->had_trigger &&
          g_run_ctx->owner_tid != std::this_thread::get_id()))
        return;   // dispatch thread, or no triggered run in this thread's lineage
    static constexpr const char* kMsg =
        "[xinsp2] current_trigger() called off the inspect thread — read the "
        "trigger ON the inspect thread and capture into the parallel body "
        "(use xi::trigger_snapshot() / xi::parallel_for; see the Parallelism "
        "section of docs/guides/write-a-script.md)";
#ifndef NDEBUG
    std::fprintf(stderr, "FATAL: %s\n", kMsg);
    std::fflush(stderr);
    std::abort();
#else
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "ERROR: %s\n", kMsg);
#endif
}

// J4: WRITE-path analogue of the READ guard above — is this use(sink).push()
// running off the dispatch thread? push() STAGES into the dispatch thread's
// thread_local g_staged, which is drained ONLY on that thread (drain_/flush_
// staged_emits_); a push from a spawned worker would stage into THAT worker's
// never-flushed g_staged (leaked ref + dropped delivery). Staging genuinely
// cannot be made worker-safe here, so the rejection is PRESERVED — but now driven
// off the A4 RunContext, not the retired marker: the context carries owner_tid
// (the dispatch thread that owns g_staged) and is propagated by value onto
// workers, so a tid mismatch means "spawned worker" for BOTH triggered and
// timer-tick / cmd:run dispatch threads (each installs its own owner_tid). Off a
// run entirely (g_run_ctx == null) nothing is staged, so allow. A DETACHED SNAPSHOT
// (a spawn_worker context — result_slot nulled) is rejected on the result_slot
// marker FIRST (structural — independent of the owner_tid comparison). Wave-2 #5:
// the snapshot now keeps the PARENT's owner_tid (A4 symmetry — the READ guard
// warn_trigger_off_thread_ needs the tid mismatch to fire on a spawn_worker like
// on async/parallel_for); this rejection never depended on the worker-tid stamp.
static bool push_off_dispatch_thread_() {
    if (!g_run_ctx) return false;               // off any run: nothing staged here
    if (!g_run_ctx->result_slot) return true;   // detached snapshot (spawn_worker): never flushed → reject
    return g_run_ctx->owner_tid != std::this_thread::get_id();
}

// Once-per-sink-name loud rejection of an off-dispatch-thread push. Mirrors
// warn_trigger_off_thread_ (abort in Debug; warn-once in Release), but keyed per
// SINK NAME so each mis-wired sink surfaces once. The push was NOT retained and the
// callback returns -6 (script push() → false), so a dropped delivery is now a
// visible programming error instead of a silent leak.
static void warn_push_off_thread_(const char* name) {
    std::string key = name ? name : "";
#ifdef NDEBUG
    // E1 (burr audit): check-then-build, matching the xi_use.hpp warn-once
    // siblings — the message string is only assembled on the FIRST (warning)
    // pass per sink name; the steady-state repeat path does the map probe only.
    static std::mutex mu;
    static std::unordered_map<std::string, bool> warned;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!warned.emplace(key, true).second) return;   // warned this sink already
    }
#endif
    std::string msg =
        "xi::use(\"" + key + "\").push(pack) called off the inspect/dispatch thread — "
        "push() stages into the dispatch thread's ordered-sink queue, which is never "
        "flushed on a xi::async / xi::parallel_for worker (the ref would leak and the "
        "delivery drop). Push ON the inspect thread — capture the pack and push after "
        "the parallel region, not from inside it.";
#ifndef NDEBUG
    std::fprintf(stderr, "FATAL: %s\n", msg.c_str());
    std::fflush(stderr);
    std::abort();
#else
    std::fprintf(stderr, "ERROR: %s\n", msg.c_str());
#endif
}

// Release every host resource a finished trigger event owns: the sealed payload
// pack ref (via the installed PackRegistry releaser). Call exactly once per event
// when it's done — dispatched or dropped — mirroring the bus's own per-drop-site
// discipline so a pack carried on the bus can't leak. (THE CUT: the Record-era
// image-handle + metadata-doc releases are gone with those TriggerEvent members.)
void release_trigger_event_(xi::TriggerEvent& ev) {   // decl in header (cross-TU)
    // release_pack_ routes to the installed PackRegistry releaser; XI_PACK_NULL
    // after so a double call can't re-release. No-op for a non-payload event.
    if (ev.pack != XI_PACK_NULL) {
        xi::TriggerBus::instance().release_pack_(ev.pack);
        ev.pack = XI_PACK_NULL;
    }
}

// RAII for "this thread's inspect sees `ev` as its trigger". Sets g_current_trigger
// for the scope; on destruction clears it back to nullptr AND releases the event's
// image/meta handles. TriggerEvent has no destructor (the discipline is manual), so
// every dispatch site used to hand-write `g_current_trigger = &ev; run; = nullptr;
// release_trigger_event_(ev);`. If run_one_inspection unwound between the set and the
// manual clear (a translated SEH / bad_alloc escaping the use() boundary),
// g_current_trigger was left dangling at a popped-stack ev (the next current_trigger
// callback = UAF) and the event leaked. The dtor makes both impossible and a new
// dispatch site can't get the sequence wrong.
// CurrentTriggerScope declared in service_state.hpp; ctor/dtor defined here
// (they touch the file-local release_trigger_event_).
CurrentTriggerScope::CurrentTriggerScope(xi::TriggerEvent& ev) : ev_(ev) {
    g_current_trigger = &ev;
    // F4: the "this run carried a trigger" signal now rides the A4 RunContext
    // (RunContextScope, installed by run_inspection_compute_ with had_trigger =
    // g_current_trigger != null) instead of a separate marker set here — so the
    // scopes stay decoupled and there is one carrier. g_current_trigger remains
    // thread_local + not propagated (its off-thread nullness is what the read
    // guard keys on).
}
CurrentTriggerScope::~CurrentTriggerScope() {
    g_current_trigger = nullptr;
    release_trigger_event_(ev_);
}

// F7: RAII for the enqueue path's "release the event UNLESS it was handed off to a
// lane queue" discipline. enqueue_to_lane_ has several early-return / drop exits that
// must release, and a couple of success exits that move `ev` into the queue and must
// NOT. The old hand-written `rel()` made FORGETTING = leak; this inverts it —
// release-by-default, call dismiss() only where ownership is transferred. (A move
// leaves `ev` empty, so even a missed dismiss() releases nothing — never a
// double-free.) Same shape as DispatchPoolGuard.
// TriggerEventReleaser moved to service_state.hpp (used by dispatch TU).

// ---- staged-sink drain / flush (paired with use_push_pack_cb staging above) -----
// Drain WITHOUT delivering — release every staged item's pack ref. The backstop for
// paths that staged but won't flush (no script, inspect crash, early return):
// StagedEmitGuard runs it on scope exit.
void drain_staged_emits_() {   // decl in header
    for (auto& it : g_staged) release_trigger_event_(it.rec);
    g_staged.clear();
}
// StagedEmitGuard struct moved to service_state.hpp.

// Deliver every staged sink call, in call order, to its target's process() via the
// SEH-guarded inline path. Called inside the EmitTurn gate (after wait_turn) so the
// deliveries are serialized in frame order. Stamps the run/arrival id ($seq) onto
// each record so a sink can correlate the packet to its frame. Fire-and-forget: the
// reply is dropped. On return g_staged is empty so StagedEmitGuard then no-ops.
void flush_staged_emits_(int64_t run_id) {   // decl in header
    (void)run_id;   // THE CUT: only pack pushes are staged now; a sealed pack is
                    // delivered AS-IS with no host-side $seq stamping (see below).
    // Move out first so g_staged is empty BEFORE any release: a throw mid-flush must
    // not let StagedEmitGuard re-release an item we already freed (worst case: a leak
    // of the not-yet-flushed tail under OOM, never a double-free).
    std::vector<StagedEmit> staged = std::move(g_staged);
    g_staged.clear();
    for (auto& it : staged) {
        // polaris2 gate P2: a staged PACK push (use(sink).push(pack)). The sealed
        // pack is delivered AS-IS — no $seq stamping (immutable; and byte-identity
        // between this push and a direct host-side dump of the same pack is the
        // contract — the pack's own $channel/$seq entries carry routing/ordering).
        // U3 contract (docs/new_gen/17): delivery ORDER here — inside the EmitTurn
        // gate, in staging order — is the envelope's authoritative guarantee to
        // the sink; in-band IDENTITY is producer-stamped before seal ($seq =
        // xi::run_id() for the host arrival id). The envelope never backfills
        // entries.
        // use_push_pack_inline_ owns the SEH boundary + fault policy; -1/-3/-4
        // need no ref rebalance here because the door BORROWS the handle either
        // way. release_trigger_event_ drops our staged ref on every path.
        if (it.rec.pack != XI_PACK_NULL)
            use_push_pack_inline_(it.target.c_str(), it.rec.pack);
        release_trigger_event_(it.rec);   // drops our staged pack ref on every path
    }
    // B4 (burr audit): hand the local's CAPACITY back to g_staged so the next
    // frame's push() doesn't re-allocate from zero (the move-out above stripped
    // it every frame). Every element was released in the loop, so clear() drops
    // no live ref. Guarded on g_staged.empty(): staging during the flush can't
    // happen today (use(...).push is script-only and a sink's pack door can't
    // re-enter the script), but if that ever changes a re-staged item must not
    // be swapped away. On a mid-flush throw we skip the swap — losing capacity
    // is fine; the throw-safety the move-out bought is unchanged.
    staged.clear();
    if (g_staged.empty()) g_staged.swap(staged);
}

// CurrentTriggerInfoC struct moved to service_cmds.hpp.
void trigger_info_cb(CurrentTriggerInfoC* out) {
    if (!out) return;
    if (!g_current_trigger) { warn_trigger_off_thread_(); *out = {{0,0}, 0, 0, 0, 0}; return; }
    out->id             = g_current_trigger->id;
    out->timestamp_us   = g_current_trigger->timestamp_us;
    out->is_active      = 1;
    out->_pad           = 0;
    out->dequeued_at_us = g_current_trigger->dequeued_at_us;
}

// THE CUT: the Record trigger-access script callbacks — trigger_image_cb,
// trigger_sources_cb, trigger_leader_cb, trigger_meta_cb — are DELETED. They read
// the TriggerEvent's Record-era image map / leader_source / metadata doc, which no
// longer exist; scripts now read the frame payload through t.pack() off the pack
// plane. trigger_info_cb (identity/timestamp only) stays. The wiring that installed
// these (set_trigger_*_callback in service_cmd_lifecycle.cpp, decls in
// service_cmds.hpp) is reconciled with THE CUT.

// ---- Image-pool owner get/set thunks (C1) ----------------------------------
// Bridge the backend's ImagePool owner thread_local across the ABI seam so SDK
// code (xi::async / xi::parallel_for) can carry the owner onto worker threads.
// owner_set is a plain assignment, NOT an OwnerGuard — the SDK side wraps it in
// its own RAII (OwnerScope: capture prev via owner_get, restore on exit), so a
// raw setter is exactly the primitive it needs. Both run ON the calling thread,
// so they read/write THAT thread's owner slot — which is the whole point: a
// worker thread installs the parent's owner before it creates pool images.
uint32_t owner_get_cb() {
    return (uint32_t)xi::ImagePool::current_owner();
}
void owner_set_cb(uint32_t id) {
    xi::ImagePool::current_owner_ref() = (xi::ImagePoolOwnerId)id;
}

// The single script-facing host_api (image_* + doc_* over the live singleton
// ImagePool). Shared by set_use_callbacks (wired into the script's
// g_use_host_api_) AND the A4 explicit-trigger entry. One instance so both paths
// address the same pool/registry. THE CUT: install_trigger_hook (the Record
// emit_record/emit_trigger wiring, deleted from xi_trigger_bus.hpp) is no longer
// installed here — the data plane is the xi.pack@1 door.
const xi_host_api* script_host_api_() {
    static xi_host_api use_host = xi::ImagePool::make_host_api();
    return &use_host;
}

// ---- project-boundary reset (Wave-2 #3) --------------------------------------
// Drop every piece of engine state that belongs to the project being replaced:
//   * the bus sink + per-source emit-time map (source names are per-project;
//     a stale sink can fire into a torn-down project, and the emit-time map
//     otherwise accumulates across every open→emit→close cycle);
//   * the script replay shadows (param_cache / instance_def_cache) and the U2
//     kv channel. open/close/create_project do NOT unload the inspection script
//     DLL (script lifecycle is independent of the project's plugin DLLs), so
//     without this the next project's compile_and_load would capture the PRIOR
//     project's xi::kv() and replay the prior project's tuned param/def values
//     over any same-named declarations — running project B with project A's
//     calibration and silently mis-verdicting (the documented cross-project
//     leak class).
// Root cause of the extraction: this block was duplicated verbatim in
// cmd_open_project_ and cmd_close_project_, and MISSING from
// cmd_create_project_ (which also replaces the project) — one primitive, three
// callers, no third copy to drift.
void reset_project_boundary_state_() {   // decl in header (cross-TU)
    xi::TriggerBus::instance().clear_sink();
    xi::TriggerBus::instance().reset();
    std::lock_guard<std::mutex> lk(g_eng.script_mu);
    g_eng.param_cache.clear();
    g_eng.instance_def_cache.clear();
    g_eng.persistent_kv_bytes.clear();
    g_eng.persistent_kv_schema = 0;
}

