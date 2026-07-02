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

#include <yyjson.h>
#include <xi/xi_graph_capture.hpp>

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
    if (adapter->reinit()) {
        adapter->reset_reinit_fails();
        xi::health().clear_instance_degraded(name);   // recovered → ok / running
    } else if (adapter->note_reinit_fail() >= xi::kReinitEscalateAfter) {
        quarantine_instance_(name, adapter);
    }
}

// The inline cross-instance process() path: run the target plugin's process() NOW,
// on this thread. Used directly for a normal xi::use(x).process() (input wiring) and
// by the staged-sink flush. A sink target is intercepted by use_process_cb (below)
// and never reaches here inline mid-inspect.
//
// Return codes: >=0 output image count; -1 no such instance (input doc untouched);
// -2 process() faulted mid-call (torn output — do NOT trust it); -3 the instance is
// quarantined (on_fault=refuse) — plugin NOT entered, input doc untouched (treat the
// ref accounting like -1). See xi_use.hpp for how the script side maps these.
static int use_process_inline_(const char* name,
                          const void* input_doc,
                          const uint8_t* input_data, int32_t input_len,
                          const xi_record_image* input_images, int input_image_count,
                          xi_record_out* output) {
    auto inst = xi::InstanceRegistry::instance().find(name);
    if (!inst) return -1;

    // All plugins run in-process (process isolation removed 2026-05).
    // Check if it's a C ABI adapter with process_fn
    auto* adapter = dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get());
    if (adapter && adapter->process_fn()) {
        // item 14: refuse gate — one atomic load on the (rare-fault) hot path. A
        // quarantined instance fails fast WITHOUT entering plugin code and without
        // touching the input doc (rc -3 → the caller balances the reserved ref).
        if (adapter->quarantined()) return -3;
        // item 14: on_fault=reinit — a prior caught fault requested a rebuild; do
        // it now, before entering plugin code, so a corrupted invariant can't reach
        // this frame. The rebuild may escalate to refuse (Nth failure).
        if (adapter->reinit_pending()) {
            apply_pending_reinit_(name, adapter);
            if (adapter->quarantined()) return -3;
        }
        // G2.1 — stamp the crash culprit before entering plugin code. If this
        // process() faults, the FE's crash report names this plugin (cross-checked
        // against the faulting module) and can quarantine it.
        stamp_culprit_(name, inst->plugin_name());
        xi_record in_rec{};   // zero-init so the v3 `doc` field is null (JSON path)
        in_rec.images = input_images;
        in_rec.image_count = input_image_count;
        // γ in-process fast path: when the target plugin shares our yyjson
        // layout, hand it the borrowed doc directly (zero serialize / zero
        // parse). Otherwise serialise the doc to JSON HERE (the caller skipped
        // data_json) so a foreign/older plugin still gets valid bytes. Owns the
        // serialized buffer for the duration of the call.
        std::string in_js;
        // Q0f: true iff we hand the plugin a BORROWED doc with the adopter ref
        // UNRELEASED. On a caught crash below (rc -2) that reserved ref is
        // deliberately leaked (leak-over-UAF), so this is exactly the condition
        // under which the leak counter must tick.
        bool borrowed_doc_ref = false;
        if (input_doc && adapter->doc_input_ok()) {
            in_rec.doc = input_doc;
            borrowed_doc_ref = true;
        } else if (input_doc) {
            size_t jl = 0;
            char* js = yyjson_mut_write((yyjson_mut_doc*)input_doc, 0, &jl);
            if (js) { in_js.assign(js, jl); free(js); }
            in_rec.data = (const uint8_t*)in_js.data();
            in_rec.len  = (int32_t)in_js.size();
            // γ-4: UseProxy share_out'd this input and reserved a ref for an
            // adopter; this JSON-fallback target serializes instead of adopting,
            // so balance that reserved ref. No-op if it wasn't registry-managed.
            xi::DocRegistry::instance().release((yyjson_mut_doc*)input_doc);
        } else {
            in_rec.data = input_data;   // explicit-JSON caller (in_doc null)
            in_rec.len  = input_len;
        }
        // adapter->process() owns the owner_id tagging (image-leak sweep) AND,
        // for a non-reentrant plugin, the per-instance lock that serializes
        // concurrent dispatch workers. We keep the SEH try/catch boundary here.
        try {
            int rc = adapter->process(&in_rec, output);
            // Graph capture (off by default): record this call's image handles
            // for dataflow-edge reconstruction. Handles are still valid here —
            // the script side adopts/releases the outputs after we return.
            if (rc >= 0 && xi::GraphCapture::instance().enabled())
                xi::GraphCapture::instance().record(name, input_images, input_image_count, output);
            return rc;
        } catch (const seh_exception& e) {
            std::fprintf(stderr, "[xinsp2] use_process('%s') crashed: 0x%08X (%s)\n",
                         name, e.code, e.what());
            char why[96]; std::snprintf(why, sizeof(why), "process() crashed: 0x%08X", e.code);
            note_instance_crash_(name, why);   // crash-loop count + health degraded
            apply_on_fault_policy_(name, adapter);   // item 14: reuse / reinit / refuse
            // This SEH boundary SWALLOWS the fault (returns -2) and the running inspect
            // continues on THIS lane worker: a STACK_OVERFLOW here would hole the guard
            // page and the rest of the inspect would run on a compromised stack. Restore
            // it (or hard-exit for respawn) before returning to the script.
            xi::recover_seh_stack_or_die(e.code, "plugin process()");
            // Q0f: a torn call may or may not have adopted the borrowed doc's reserved
            // ref; we don't second-guess it (leak-over-UAF), so the ref is leaked. Count
            // it (dispatch_stats.crash_leaked_docs_lifetime). Every service-side crash of
            // a doc-carrying process() — synchronous OR staged-sink flush — funnels here.
            if (borrowed_doc_ref) xi::DocRegistry::instance().note_crash_leak();
            return -2;
        } catch (...) {
            std::fprintf(stderr, "[xinsp2] use_process('%s') threw exception\n", name);
            note_instance_crash_(name, "process() threw an exception");
            apply_on_fault_policy_(name, adapter);   // item 14: reuse / reinit / refuse
            if (borrowed_doc_ref) xi::DocRegistry::instance().note_crash_leak();   // Q0f: leaked ref
            return -2;
        }
    }
    return -1;
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

// Stage a sink call: adopt the input's doc + image refs so they outlive use()'s
// return (the SDK releases its own refs right after we return), then queue it.
// Mirrors install_trigger_hook / the bus adopt logic. Returns 0 with an empty reply
// (sinks are fire-and-forget — the script ignores the return Record).
static int stage_sink_emit_(const char* name, const void* input_doc,
                            const uint8_t* input_data, int32_t input_len,
                            const xi_record_image* input_images, int input_image_count,
                            xi_record_out* output) {
    if (output) xi_record_out_init(output);   // empty reply
    StagedEmit item;
    item.target = name;
    if (input_doc) {
        // take the share_out'd ref (already reserved for us — adopt, no retain)
        item.rec.meta_doc = xi::DocRef::adopt((yyjson_mut_doc*)(void*)input_doc);
    } else if (input_data && input_len > 0) {
        yyjson_doc* idoc = yyjson_read((const char*)input_data, (size_t)input_len, 0);
        if (idoc) {
            yyjson_mut_doc* m = yyjson_doc_mut_copy(idoc, nullptr);
            yyjson_doc_free(idoc);
            if (m) {
                xi::DocRegistry::instance().addref(m);   // register at rc=1
                item.rec.meta_doc = xi::DocRef::adopt(m);
            }
        }
    }
    // Preserve the record's ORIGINAL image keys exactly — staging replaces an inline
    // use().process() call, so the sink must see the same keys ("inverted", "edges",
    // …) the inline path would have delivered. (NOT the bus lone-image→source-name
    // convention; that's for emit_trigger, not use(sink).process().)
    for (int i = 0; input_images && i < input_image_count; ++i) {
        xi_image_handle h = input_images[i].handle;
        xi::ImagePool::instance().addref(h);
        std::string key = (input_images[i].key && input_images[i].key[0])
                            ? std::string(input_images[i].key) : ("img" + std::to_string(i));
        if (!item.rec.images.emplace(std::move(key), h).second)
            xi::ImagePool::instance().release(h);   // dup/empty key: drop the extra ref
    }
    g_staged.push_back(std::move(item));
    return 0;
}

// xi::use().process() entry wired into the script DLL. A declared ORDERED SINK target
// is staged (frame-ordered flush); every other target runs inline as before.
int use_process_cb(const char* name,
                          const void* input_doc,
                          const uint8_t* input_data, int32_t input_len,
                          const xi_record_image* input_images, int input_image_count,
                          xi_record_out* output) {
    if (name) {
        if (auto inst = xi::InstanceRegistry::instance().find(name)) {
            auto* a = dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get());
            if (a && a->is_sink())
                return stage_sink_emit_(name, input_doc, input_data, input_len,
                                        input_images, input_image_count, output);
        }
    }
    return use_process_inline_(name, input_doc, input_data, input_len,
                              input_images, input_image_count, output);
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

// A1: owning thread id of the in-flight CurrentTriggerScope — NON-thread-local
// (unlike g_current_trigger above) so any thread can tell "is a trigger active
// somewhere?" apart from "is one active on MY thread?". GetCurrentThreadId() is
// never 0 for a live thread, so 0 unambiguously means "no trigger in flight".
//
// This disambiguates the two cases a trigger thunk's `!g_current_trigger` branch
// used to conflate (see Problem A in docs/internals/core_fix_plan.md):
//   * g_eng.inspect_tid == 0  → genuinely no trigger (plain cmd:run, timer tick):
//     keep the historical empty / XI_IMAGE_NULL semantics.
//   * g_eng.inspect_tid != 0  → a trigger IS active, but the caller is on a DIFFERENT
//     thread — an xi::async task or #pragma omp body that read the ambient
//     trigger off the inspect thread. That is the silent-bug class; fail loud.

// A1: invoked from a trigger thunk's "no current trigger" branch. If a trigger is
// actually in flight (on another thread), the caller used current_trigger() off
// the inspect thread — abort with a named message in debug, log-once in release.
// If no trigger is in flight at all, returns quietly so the thunk preserves its
// pre-existing empty / XI_IMAGE_NULL semantics (legitimate cmd:run / timer paths).
static void warn_trigger_off_thread_() {
    if (g_eng.inspect_tid.load(std::memory_order_acquire) == 0) return;   // genuinely no trigger
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

// Release every host resource a finished trigger event owns: image handle refs
// (ImagePool) + the ABI-v5 metadata doc ref (DocRegistry). Call exactly once
// per event when it's done — dispatched or dropped — mirroring the bus's own
// per-drop-site discipline so a metadata doc carried on the bus can't leak.
void release_trigger_event_(xi::TriggerEvent& ev) {   // decl in header (cross-TU)
    for (auto& [s, h] : ev.images) xi::ImagePool::instance().release(h);
    ev.meta_doc.reset();   // release the event's doc ref + null it (dtor then no-ops)
    // polaris2 wave-2: drop the event's Frame ref too (no-op for a Record-era
    // event, whose frame is XI_FRAME_NULL). release_frame_ routes to the installed
    // FrameRegistry releaser; XI_FRAME_NULL after so a double call can't re-release.
    if (ev.frame != XI_FRAME_NULL) {
        xi::TriggerBus::instance().release_frame_(ev.frame);
        ev.frame = XI_FRAME_NULL;
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
    // A1: publish the owning thread id so a trigger thunk fired on another
    // thread can tell "wrong thread" (loud bug) from "no trigger" (legit).
    g_eng.inspect_tid.store(GetCurrentThreadId(), std::memory_order_release);
}
CurrentTriggerScope::~CurrentTriggerScope() {
    g_eng.inspect_tid.store(0, std::memory_order_release);
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

// ---- staged-sink drain / flush (paired with stage_sink_emit_ above) -------------
// Drain WITHOUT delivering — release every staged item's image/doc refs. The
// backstop for paths that staged but won't flush (no script, inspect crash, early
// return): StagedEmitGuard runs it on scope exit.
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
    // Move out first so g_staged is empty BEFORE any release: a throw mid-flush must
    // not let StagedEmitGuard re-release an item we already freed (worst case: a leak
    // of the not-yet-flushed tail under OOM, never a double-free).
    std::vector<StagedEmit> staged = std::move(g_staged);
    g_staged.clear();
    std::vector<xi_record_image> in_imgs;
    // RAII: xi_record_out_free (+ the consumer-ref releases the plugin handed back)
    // must run on EVERY exit path. They used to sit at the tail of the try, on the
    // happy path only — a throw between the process() call and there (e.g. a release
    // on a corrupt handle, or bad_alloc) jumped to the catch and skipped them, leaking
    // the out_doc + pool handles. Same cleanup-by-default shape as copy_ref /
    // TriggerEventReleaser. Dtor swallows: cleanup must not throw during unwinding.
    struct RecordOutGuard {
        xi_record_out* out;
        bool release_refs = false;   // armed once prc>=0: the returned refs are ours to drop
        explicit RecordOutGuard(xi_record_out* o) : out(o) {}
        ~RecordOutGuard() {
            try {
                if (release_refs) {
                    if (out->out_doc)
                        xi::DocRegistry::instance().release((yyjson_mut_doc*)out->out_doc);
                    for (int i = 0; i < out->image_count; ++i)
                        if (out->images[i].handle)
                            xi::ImagePool::instance().release(out->images[i].handle);
                }
            } catch (...) { /* releases are effectively noexcept; never let one propagate */ }
            xi_record_out_free(out);
        }
        RecordOutGuard(const RecordOutGuard&) = delete;
        RecordOutGuard& operator=(const RecordOutGuard&) = delete;
    };
    for (auto& it : staged) {
        // Non-null iff we delivered a PRIVATE COW copy this iteration: released on every
        // exit path below (declared out here so a throw mid-flush can't leak it).
        yyjson_mut_doc* copy_ref = nullptr;
        try {
            // Pick the doc to deliver. it.rec.meta_doc is registry-refcounted and may be
            // SHARED: a script can stage the SAME Record to several sinks (each share_out's
            // the one underlying doc), or build the record from a borrowed trigger/plugin
            // doc still held elsewhere. Stamping $seq into a shared doc would (a) mutate a
            // doc a concurrent holder reads and (b) double-stamp when two staged items point
            // at it. So COW only when actually shared (rc>1); the common single-sink path
            // (rc==1, sole owner) stamps in place with no copy — speed-first. (rc can only
            // fall, never rise, behind our back here: we hold the sole non-shared ref and
            // don't hand the doc out before stamping, so the rc==1 fast path is safe.)
            yyjson_mut_doc* deliver = it.rec.meta_doc.get();
            if (deliver && xi::DocRegistry::instance().refcount(deliver) > 1) {
                if (yyjson_mut_doc* copy = yyjson_mut_doc_mut_copy(deliver, nullptr)) {
                    xi::DocRegistry::instance().addref(copy);   // register at rc=1 (our ref)
                    deliver  = copy;
                    copy_ref = copy;
                }
                // Copy failed (OOM): fall through and stamp the original — a best-effort
                // $seq beats dropping the frame; the duplicate-key risk is the lesser evil.
            }
            // Frame correlation: stamp the arrival/run id with PUT semantics — remove any
            // existing $seq first so a re-stamp (or a doc that already carried $seq) can't
            // accumulate duplicate keys. Best-effort — skip if the doc isn't an object.
            if (deliver) {
                yyjson_mut_val* root = yyjson_mut_doc_get_root(deliver);
                if (root && yyjson_mut_is_obj(root)) {
                    yyjson_mut_obj_remove_str(root, "$seq");
                    yyjson_mut_obj_add_int(deliver, root, "$seq", run_id);
                }
            }
            in_imgs.clear();
            in_imgs.reserve(it.rec.images.size());
            for (auto& [k, h] : it.rec.images) in_imgs.push_back(xi_record_image{k.c_str(), h});
            // Reserve one doc ref for the consumer (adopt or serialize-release both
            // CONSUME one); our own ref (original via release_trigger_event_ below, or the
            // COW copy via copy_ref) balances the other.
            if (deliver) xi::DocRegistry::instance().addref(deliver);
            xi_record_out output; xi_record_out_init(&output);
            RecordOutGuard out_guard{&output};   // frees + drops the returned refs on ALL paths
            int prc = use_process_inline_(it.target.c_str(), deliver, nullptr, 0,
                                          in_imgs.data(), (int)in_imgs.size(), &output);
            // prc == -1 (target gone) / -3 (quarantined, item 14): the input doc was
            // never touched → our reserved ref wasn't consumed; release it. prc == -2
            // (crash) may have — don't second-guess a torn call (mirrors xi_use.hpp).
            // The leaked ref is COUNTED once, inside use_process_inline_'s crash catch
            // above (Q0f) — this flush shares that funnel, so we must NOT count again here.
            if (deliver && (prc == -1 || prc == -3)) xi::DocRegistry::instance().release(deliver);
            // Arm the guard to also drop the out_doc + output image refs (prc>=0 only —
            // a torn/crashed call's output is untrustworthy; leave its refs alone).
            if (prc >= 0) out_guard.release_refs = true;
        } catch (const seh_exception& e) {
            std::fprintf(stderr, "[xinsp2] sink '%s' flush crashed: 0x%08X (%s)\n",
                         it.target.c_str(), e.code, e.what());
            // Loop keeps flushing later staged sinks on this same worker thread.
            xi::recover_seh_stack_or_die(e.code, "sink flush");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[xinsp2] sink '%s' flush threw: %s\n", it.target.c_str(), e.what());
        } catch (...) {
            std::fprintf(stderr, "[xinsp2] sink '%s' flush threw a non-std exception\n", it.target.c_str());
        }
        // Our COW ref (if any) — the consumer ref was reserved+consumed above; this drops
        // OUR ref, freeing the copy. The original meta_doc is released untouched next.
        if (copy_ref) xi::DocRegistry::instance().release(copy_ref);
        release_trigger_event_(it.rec);   // our owned image + ORIGINAL doc refs — every path
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

xi_image_handle trigger_image_cb(const char* source) {
    if (!g_current_trigger) { warn_trigger_off_thread_(); return XI_IMAGE_NULL; }
    if (!source) return XI_IMAGE_NULL;
    auto it = g_current_trigger->images.find(source);
    if (it == g_current_trigger->images.end()) {
        // Reader-side sole-image fallback (cold path — only on an exact-key
        // MISS, so the hot emit path stays allocation-free with NO second map
        // entry per frame). A single-image event resolves by ANY key: the
        // record's own key (e.g. "frame" — the documented contract + cmd:run
        // inject) AND the emitter instance name (legacy reads) both land on the
        // lone frame. A multi-image event keeps strict exact-key routing.
        if (g_current_trigger->images.size() == 1)
            it = g_current_trigger->images.begin();
        else
            return XI_IMAGE_NULL;
    }
    // Caller (script) releases via host_api->image_release after copying
    // pixels — addref so our own release on dispatch-end doesn't free it
    // out from under them.
    xi::ImagePool::instance().addref(it->second);
    return it->second;
}

int32_t trigger_sources_cb(char* buf, int32_t buflen) {
    if (!g_current_trigger) { warn_trigger_off_thread_(); return 0; }
    if (!buf) return 0;
    std::string out;
    bool first = true;
    for (auto& [src, h] : g_current_trigger->images) {
        if (!first) out.push_back('\n');
        first = false;
        out += src;
    }
    int32_t n = (int32_t)out.size();
    if (buflen < n + 1) return -n;
    std::memcpy(buf, out.data(), n);
    buf[n] = 0;
    return n;
}

// P2-2: expose TriggerEvent::leader_source to scripts. leader_source is
// simply the emitting instance's name (the source that emit_record'd this
// event); scripts consult sources() for the full set. Same -needed_bytes
// convention as trigger_sources_cb so scripts can resize and retry.
int32_t trigger_leader_cb(char* buf, int32_t buflen) {
    if (!g_current_trigger) { warn_trigger_off_thread_(); return 0; }
    if (!buf) return 0;
    const std::string& s = g_current_trigger->leader_source;
    int32_t n = (int32_t)s.size();
    if (n == 0) return 0;
    if (buflen < n + 1) return -n;
    std::memcpy(buf, s.data(), n);
    buf[n] = 0;
    return n;
}

// ABI v5: hand the script the event's metadata doc (emit_trigger_record) as a
// borrowed read-only view — zero-serialize. We RESERVE one ref (retain) for the
// script's adopt_shared to consume, exactly as Record::share_out reserves a ref
// for record_from_c's adopt_shared on the process()-input doc. The script-side
// xi::Trigger::meta() adopt_shared's it and doc_release's when its Record dies,
// balancing this reserve; the worker still holds the event's own ref until
// release_trigger_event_. Returns null when the trigger carries no metadata.
void* trigger_meta_cb() {
    if (!g_current_trigger) { warn_trigger_off_thread_(); return nullptr; }
    if (!g_current_trigger->meta_doc) return nullptr;
    xi::DocRegistry::instance().addref(g_current_trigger->meta_doc.get());
    return (void*)g_current_trigger->meta_doc.get();
}

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
// ImagePool, with the trigger/emit hook installed). Shared by set_use_callbacks
// (wired into the script's g_use_host_api_) AND the A4 explicit-trigger entry
// (put into the xi_trigger_view so the SDK can resolve the passed image/meta
// handles). One instance so both paths address the same pool/registry.
const xi_host_api* script_host_api_() {
    static xi_host_api use_host = [] {
        auto a = xi::ImagePool::make_host_api();
        xi::install_trigger_hook(a);
        return a;
    }();
    return &use_host;
}

