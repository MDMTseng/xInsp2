//
// service_sinks.cpp — the script host_api callback surface: pipeline graph
// capture glue, ordered output sinks (stage/drain/flush), trigger-loop state,
// trigger-access script callbacks, image-pool owner thunks, watchdog-slot
// arm/disarm, and script_host_api_. Split from service_main.cpp
// (behavior-preserving; see service_internal.hpp).
//
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <xi/xi_pack_abi.hpp>   // PackRegistry retain/release (use_push_pack_cb)
#include <xi/xi_pack_contract.hpp> // U1 fault short-circuit (use_pack_process_cb, doc 15)

#include "service_internal.hpp"

// ---- Pipeline graph capture (stage 2) ----------------------------------
// Opt-in dataflow-graph capture lives in xi_graph_capture.hpp (xi::GraphCapture
// singleton). OFF by default → the hot path below pays only a relaxed atomic
// load. The ws handlers (graph_capture / graph_snapshot) drive it.

// Defined after g_eng.plugin_mgr (declared further down); records a per-instance
// process() crash so a crash loop is visible via get_state.
// note_instance_crash_ declared in service_internal.hpp.

// Part III G2.1 — stamp the process-global crash culprit (xi::crash::g_culprit)
// with the instance/plugin the host is about to enter, plus that plugin's
// folder + dll so the FE can quarantine it on a death. Defined after
// g_eng.plugin_mgr. Cheap on the dispatch hot path: a per-thread cache means the
// manager lock is taken only when the active plugin on this thread changes.
// stamp_culprit_ declared in service_internal.hpp.

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
static void apply_on_fault_policy_(const char* name, xi::CAbiInstanceAdapter* adapter) {
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
static void apply_pending_reinit_(const char* name, xi::CAbiInstanceAdapter* adapter) {
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
// StagedEmit struct moved to service_internal.hpp; g_staged DEFINED here.
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
    if (adapter->quarantined()) return -3;
    if (adapter->reinit_pending()) {
        apply_pending_reinit_(name, adapter);
        if (adapter->quarantined()) return -3;
    }
    stamp_culprit_(name, inst->plugin_name());
    try {
        xi_pack_handle ack = adapter->run_pack_door(pack);
        if (ack != XI_PACK_NULL) xi::PackRegistry::instance().release(ack);
        return 0;
    } catch (const seh_exception& e) {
        std::fprintf(stderr, "[xinsp2] use(\"%s\").push(pack) crashed: 0x%08X (%s)\n",
                     name, e.code, e.what());
        char why[96]; std::snprintf(why, sizeof(why), "pack door crashed: 0x%08X", e.code);
        note_instance_crash_(name, why);
        apply_on_fault_policy_(name, adapter);
        xi::recover_seh_stack_or_die(e.code, "plugin pack door");
        return -2;
    } catch (...) {
        std::fprintf(stderr, "[xinsp2] use(\"%s\").push(pack) threw exception\n", name);
        note_instance_crash_(name, "pack door threw an exception");
        apply_on_fault_policy_(name, adapter);
        return -2;
    }
}

// J4: reject a use(sink).push() issued off the dispatch thread. Defined below,
// next to the READ-path guard (warn_trigger_off_thread_) whose g_trigger_ctx_ /
// g_current_trigger markers they reuse — forward-declared here so use_push_pack_cb
// can call them before those thread-context markers are defined.
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
        // L2: retain_untagged, NOT the owner-tagged retain(). This staged ref is
        // the exact analogue of f_emit_pack's dispatch-event ref — taken here on
        // the dispatch thread (where current_owner() is the SCRIPT owner S via the
        // service_inspect OwnerGuard) but RELEASED off-guard by the bus releaser
        // (release_trigger_event_ → f_release_for_bus → release_as(pack, 0)), which
        // is deliberately unattributable. An owner-tagged retain would charge bucket
        // S while the untagged release can't decrement it, stranding a phantom S
        // bucket (transient Σbuckets ≤ rc over-count — diagnostic only, fail-closed,
        // never an over-release). retain_untagged keeps both sides untagged so they
        // balance, mirroring f_emit_pack's deliberate choice for the same reason.
        xi::PackRegistry::instance().retain_untagged(pack);
        StagedEmit item;
        item.target   = name;
        item.rec.pack = pack;
        g_staged.push_back(std::move(item));
        return 0;
    }
    return use_push_pack_inline_(name, pack);
}

int use_exchange_cb(const char* name, const char* cmd,
                           char* rsp, int rsplen) {
    try {
        auto inst = xi::InstanceRegistry::instance().find(name);
        if (!inst) return -1;
        std::string result = inst->exchange(cmd);
        int n = (int)result.size();
        if (rsplen < n + 1) return -n;
        std::memcpy(rsp, result.data(), result.size());
        rsp[result.size()] = 0;
        return n;
    } catch (const seh_exception& e) {
        std::fprintf(stderr, "[xinsp2] use_exchange('%s') crashed: 0x%08X (%s)\n",
                     name, e.code, e.what());
        // Swallowed on a surviving thread (lane worker under a script's use().exchange(),
        // or the command thread) — restore the stack guard page after an overflow.
        xi::recover_seh_stack_or_die(e.code, "plugin exchange()");
        return -1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[xinsp2] use_exchange('%s') threw: %s\n", name, e.what());
        return -1;
    }
}

// grab() was the legacy pull model (xi::ImageSource queue). Sources now PUSH via
// emit_record and scripts read current_trigger(), so there's nothing to grab.
xi_image_handle use_grab_cb(const char* /*name*/, int /*timeout_ms*/) {
    return XI_IMAGE_NULL;
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
    if (adapter->quarantined()) return -3;
    if (adapter->reinit_pending()) {
        apply_pending_reinit_(name, adapter);
        if (adapter->quarantined()) return -3;
    }
    if (!adapter->has_pack_door()) return -4;      // Record-only plugin
    stamp_culprit_(name, inst->plugin_name());
    try {
        *out = adapter->run_pack_door(in);         // OwnerGuard + CallScope inside
        return 0;
    } catch (const seh_exception& e) {
        std::fprintf(stderr, "[xinsp2] use_pack_process('%s') crashed: 0x%08X (%s)\n",
                     name, e.code, e.what());
        char why[96]; std::snprintf(why, sizeof(why), "pack door crashed: 0x%08X", e.code);
        note_instance_crash_(name, why);
        apply_on_fault_policy_(name, adapter);
        // Same rationale as use_process_inline_: this boundary swallows the fault
        // and the inspect continues on this worker — restore the guard page (or
        // hard-exit for respawn) before returning to the script.
        xi::recover_seh_stack_or_die(e.code, "plugin pack door");
        *out = XI_PACK_NULL;                       // a torn result is never handed out
        return -2;
    } catch (...) {
        std::fprintf(stderr, "[xinsp2] use_pack_process('%s') threw exception\n", name);
        note_instance_crash_(name, "pack door threw an exception");
        apply_on_fault_policy_(name, adapter);
        *out = XI_PACK_NULL;
        return -2;
    }
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
// crash_set / crash_set_phase are defined inline in service_internal.hpp.

// Watchdog (P2.4). When > 0, inspect() calls have this many ms of wall-
// clock budget. Default 0 = disabled (back-compat). Set via
// cmd:set_watchdog_ms or --watchdog=N.
//
// Per-worker deadlines: the parallel dispatch pool (parallelism.dispatch_threads
// > 1) runs N inspects at once, so the watchdog tracks a SLOT per in-flight
// inspect (each arms a free slot on entry, clears it on exit). The monitor scans
// all slots. On a deadline breach it first asks the script to cancel cooperatively
// (a GLOBAL flag — under N>1 this aborts every in-flight frame, which is the
// intended "something's wedged, bail this round" signal); if the script ignores
// that for the grace window, the process is unrecoverable (a forced thread kill
// would leak the per-instance lock + risk heap corruption), so the backend
// exits and the FE supervisor respawns a clean one. See docs/guides/writing-a-
// script.md (Parallel dispatch) + internals/fe-be.md.
// Per-slot inspect deadline (steady_clock epoch-ms); 0 = free. Written by the
// dispatch/run thread that owns the slot, read by the watchdog thread.
// WATCHDOG_EXIT_CODE moved to service_internal.hpp.

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

// F4: per-thread trigger-context marker — "this thread is INSIDE, or a CHILD of,
// a trigger-bearing inspect". THREAD-LOCAL and RELATIONAL (this replaces the old
// A1 process-global inspect_tid, which was a single atomic holding the last
// scope's tid — a fatally unsound heuristic under max_parallel>1: a benign
// timer-tick worker on lane B calling current_trigger() would see lane A's tid !=
// 0 and abort, even though B never had a trigger; and lane B's scope storing 0
// masked a genuine off-thread misuse on lane A).
//
// The marker is set by CurrentTriggerScope on the inspect thread, and PROPAGATED
// BY VALUE into the worker threads xi::async / xi::parallel_for spawn (via the
// trigger_ctx get/set thunks, exactly like the image-pool owner). So:
//   * inspect thread of a triggered frame  → marker=1, g_current_trigger != null
//     (warn is never reached — the thunk returns the real trigger).
//   * a child worker of that inspect        → marker=1 (propagated), but
//     g_current_trigger==null (thread_local, not inherited): the script read the
//     ambient trigger off-thread instead of snapshotting → fail loud. DETECTED.
//   * a timer-tick / plain cmd:run worker    → marker=0 (its inspect had no
//     CurrentTriggerScope): reading current_trigger() is the legitimate "no
//     trigger" case → quiet, even while ANOTHER lane runs a triggered frame.
// TRADEOFF: a hand-rolled `#pragma omp` region (NOT xi::parallel_for) that reads
// the ambient trigger off-thread is no longer flagged — the marker only rides the
// blessed primitives. That is the correct trade: the old heuristic "caught" it
// only by also aborting correct programs, and TriggerSnapshot / the A4 explicit
// entry make the off-thread ambient read unnecessary anyway.
static thread_local int g_trigger_ctx_ = 0;

// F4: bridge the marker across the ABI seam for xi::async / xi::parallel_for.
uint32_t trigger_ctx_get_cb()      { return (uint32_t)g_trigger_ctx_; }
void     trigger_ctx_set_cb(uint32_t v) { g_trigger_ctx_ = (int)v; }

// F4: invoked from a trigger thunk's "no current trigger" branch. Fires ONLY when
// THIS thread is inside / a child of a trigger-bearing inspect (marker set here or
// propagated from the spawning inspect) yet the ambient trigger is null here — the
// off-thread-read silent-bug class. When the marker is 0 (genuinely no trigger on
// this thread's lineage: plain cmd:run / timer tick, on any lane) it returns
// quietly so the thunk preserves its empty / XI_IMAGE_NULL semantics.
static void warn_trigger_off_thread_() {
    if (g_trigger_ctx_ == 0) return;   // this thread's lineage has no trigger
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
// running off the dispatch thread? Detected the SAME way: g_trigger_ctx_ marks
// "this thread is inside / a child of a trigger-bearing inspect" (set by
// CurrentTriggerScope, propagated into xi::async / xi::parallel_for workers), while
// g_current_trigger is thread_local and NOT inherited. So on the real dispatch
// thread of a triggered frame g_current_trigger != null (→ allowed; its g_staged IS
// flushed after the inspect); on a timer-tick / cmd:run dispatch thread the marker
// is 0 (→ allowed; the same thread flushes). A CHILD worker of a triggered inspect
// has marker=1 but g_current_trigger==null → its g_staged is never drained → REJECT.
// Same documented blind spot as the read guard: a child of a source-less (marker-0)
// inspect is not flagged (the marker only rides the blessed primitives).
static bool push_off_dispatch_thread_() {
    return g_trigger_ctx_ != 0 && g_current_trigger == nullptr;
}

// Once-per-sink-name loud rejection of an off-dispatch-thread push. Mirrors
// warn_trigger_off_thread_ (abort in Debug; warn-once in Release), but keyed per
// SINK NAME so each mis-wired sink surfaces once. The push was NOT retained and the
// callback returns -6 (script push() → false), so a dropped delivery is now a
// visible programming error instead of a silent leak.
static void warn_push_off_thread_(const char* name) {
    std::string key = name ? name : "";
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
    static std::mutex mu;
    static std::unordered_map<std::string, bool> warned;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!warned.emplace(key, true).second) return;   // warned this sink already
    }
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
// CurrentTriggerScope declared in service_internal.hpp; ctor/dtor defined here
// (they touch the file-local release_trigger_event_).
CurrentTriggerScope::CurrentTriggerScope(xi::TriggerEvent& ev) : ev_(ev) {
    g_current_trigger = &ev;
    // F4: mark THIS thread as inside a trigger-bearing inspect. xi::async /
    // xi::parallel_for read this marker (via trigger_ctx_get_cb) at spawn time and
    // re-install it on their worker threads, so a child that reads current_trigger()
    // off-thread is caught — while a timer-tick worker on another lane (marker 0)
    // is not. Not nested (the dispatch worker runs one inspect at a time), so a
    // plain set/clear mirrors the pre-existing g_current_trigger discipline.
    g_trigger_ctx_ = 1;
}
CurrentTriggerScope::~CurrentTriggerScope() {
    g_trigger_ctx_ = 0;
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
// TriggerEventReleaser moved to service_internal.hpp (used by dispatch TU).

// ---- staged-sink drain / flush (paired with use_push_pack_cb staging above) -----
// Drain WITHOUT delivering — release every staged item's pack ref. The backstop for
// paths that staged but won't flush (no script, inspect crash, early return):
// StagedEmitGuard runs it on scope exit.
void drain_staged_emits_() {   // decl in header
    for (auto& it : g_staged) release_trigger_event_(it.rec);
    g_staged.clear();
}
// StagedEmitGuard struct moved to service_internal.hpp.

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
}

// CurrentTriggerInfoC struct moved to service_internal.hpp.
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
// service_internal.hpp) is reconciled with THE CUT.

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

