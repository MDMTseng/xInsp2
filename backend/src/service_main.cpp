//
// service_main.cpp — xinsp-backend.exe entry point (M2 skeleton).
//
// Responsibilities in this milestone:
//   - parse --port
//   - start the WS server
//   - handle cmd: ping, version, shutdown
//   - echo anything else as an error rsp
//
// M3 adds run + vars. M4 adds previews. M5 adds compile_and_load.
//

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <typeinfo>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <yyjson.h>
// (The SDK umbrella <xi/xi.hpp> is intentionally NOT included — it pulls the
//  script-side SDK headers (xi_param/xi_io/xi_state/xi_result/xi_async) the
//  backend binary never needs. Only the core headers it actually uses are
//  included below.)
#include <xi/xi_image.hpp>
#include <xi/xi_cli_args.hpp>
#include <xi/xi_jpeg.hpp>
#include <xi/xi_protocol.hpp>
#include <xi/xi_plugin_manager.hpp>
#include <xi/xi_certify.hpp>      // Part III G1: --certify-plugin child mode (scan/certification isolation)
#include <xi/xi_project.hpp>
#include <xi/xi_owner_lock.hpp>     // F5: advisory single-writer stamp on the project folder
#include <cassert>
#include <xi/xi_trigger_bus.hpp>
#include <xi/xi_inflight_runs.hpp>   // xi::InflightRuns (detached-run lifetime owner)
#include <xi/xi_emit_gate.hpp>       // xi::EmitGate / xi::EmitTurn (ordered-emit gate)
#include <xi/xi_metrics.hpp>         // OQ-7a: frame counters + latency histogram (cmd:metrics)
#include <xi/xi_script_compiler.hpp>
#include <xi/xi_toolchain.hpp>       // lens 2#6: VS toolchain / override subsystem (extracted)
#include <xi/xi_script_loader.hpp>
#include <xi/xi_ws_server.hpp>

#include <condition_variable>
#include <filesystem>
#include <thread>

#include <windows.h>

namespace xp = xi::proto;

#include "service_internal.hpp"

// The Engine struct + all shared globals/structs/constants/helpers moved to the
// PRIVATE header service_internal.hpp (behavior-preserving split). The single
// DEFINITION of g_eng lives HERE (this TU); every other service_*.cpp sees it via
// `extern Engine g_eng;` in the header.
Engine g_eng;


// Loaded user script state. When null, cmd:run returns an error.


// Persistent cross-frame state — survives DLL reloads.
// Schema version of the DLL that wrote g_eng.persistent_state_json. The
// next DLL's xi_script_state_schema_version() is compared against
// this on restore — mismatch (and both non-zero) drops the state
// rather than letting set_state default-fill into a different shape.
// 0 means "unversioned" — restore proceeds without the check.

// Cache of every successful `cmd:set_param` value the backend pushed
// into the live script. compile_and_load replays these into the new
// DLL via xi_script_set_param so user-tuned slider values aren't
// silently reset to file-scope defaults across a recompile. Keyed by
// param name → JSON-encoded scalar (number / bool / string token,
// same shape as set_param's `value` arg). Protected by g_eng.script_mu.

// Cache of every successful script-side `cmd:set_instance_def` def the
// backend pushed into the live script. Exact sibling of g_eng.param_cache:
// script-declared xi::Instance objects live in the script DLL's OWN
// registry and their file-scope ctors re-seed the SOURCE default def on
// every reload, so without replaying the operator-tuned/taught/calibrated
// def the hot-recompile loop silently reverts each instance to its source
// default. Keyed by instance name → def JSON (same shape as set_instance_def's
// `def` arg). Only the SCRIPT-instance path is cached — backend plugin-manager
// instances persist via InstanceRegistry across a script recompile. Protected
// by g_eng.script_mu like g_eng.param_cache.

// --- xi::use() callback implementations ---
// These are called FROM the script DLL back INTO the backend, routing
// process/exchange/grab to the backend's InstanceRegistry.

#include <xi/xi_use.hpp>
#include <xi/xi_seh.hpp>
#include <xi/xi_graph_capture.hpp>
#include <xi/xi_crash_dump.hpp>   // xi::crash:: minidump + breadcrumb forensics (extracted leaf)

using xi::seh_exception;
// (seh translation installed via xi::install_seh_translator() — portable shim)

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

// The inline cross-instance process() path: run the target plugin's process() NOW,
// on this thread. Used directly for a normal xi::use(x).process() (input wiring) and
// by the staged-sink flush. A sink target is intercepted by use_process_cb (below)
// and never reaches here inline mid-inspect.
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
        if (input_doc && adapter->doc_input_ok()) {
            in_rec.doc = input_doc;
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
            note_instance_crash_(name, why);   // visible via get_state (crash-loop alerting)
            return -2;
        } catch (...) {
            std::fprintf(stderr, "[xinsp2] use_process('%s') threw exception\n", name);
            note_instance_crash_(name, "process() threw an exception");
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
            // prc == -1: target gone before touching the input doc → our reserved ref
            // wasn't consumed; release it. prc == -2 (crash) may have — don't second-
            // guess a torn call (mirrors xi_use.hpp).
            if (deliver && prc == -1) xi::DocRegistry::instance().release(deliver);
            // Arm the guard to also drop the out_doc + output image refs (prc>=0 only —
            // a torn/crashed call's output is untrustworthy; leave its refs alone).
            if (prc >= 0) out_guard.release_refs = true;
        } catch (const seh_exception& e) {
            std::fprintf(stderr, "[xinsp2] sink '%s' flush crashed: 0x%08X (%s)\n",
                         it.target.c_str(), e.code, e.what());
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


// ---- Status registry -------------------------------------------------------
// Sticky last-value status per component: instance name, or "@script" for the
// inspection script. Served to the UI via cmd:status (the delivery GUARANTEE —
// clients re-pull on every connect) + a best-effort `status` push event, and
// mirrored into the per-thread crash breadcrumb so the LAST status survives a
// crash into the report.

static int64_t status_now_ms() { return xi::wall_ms(); }   // wall: status ts

// Update the latest status for `who`. Coalesces no-op repeats (same text) so a
// component setting the same string every frame doesn't spam events. Always
// mirrors into this thread's crash breadcrumb; pushes a best-effort event.
void set_status_internal(const std::string& who, const char* text) {
    std::string t = text ? text : "";
    crash_set(crash_ctx().last_status, sizeof(crash_ctx().last_status), t.c_str());
    uint64_t seq;
    {
        std::lock_guard<std::mutex> lk(g_eng.status_mu);
        auto it = g_eng.status.find(who);
        if (it != g_eng.status.end() && it->second.text == t) return;  // coalesce
        seq = ++g_eng.status_seq;
        g_eng.status[who] = StatusEntry{t, status_now_ms(), seq};
    }
    if (auto* srv = g_eng.srv_for_bp.load(std::memory_order_acquire)) {
        std::string msg = "{\"type\":\"event\",\"name\":\"status\",\"data\":{\"source\":";
        xp::json_escape_into(msg, who);
        msg += ",\"text\":";
        xp::json_escape_into(msg, t);
        msg += ",\"seq\":" + std::to_string(seq) + "}}";
        srv->send_text(msg);
    }
}

// Installed into the script DLL (xi_script_set_status_callback) so xi::status()
// in user scripts publishes under "@script".
void status_cb(const char* text) {
    set_status_internal("@script", text);
}

// ---- Per-run Result (run_result event) --------------------------------------
// One Result per trigger: a signed status code + message. See
// docs/roadmap/run-result.md. Framework system-fail enum lives in a reserved band
// (<= -990000) the user API (xi::result) refuses to set.
// XI_SYS_* enum, RunResult struct + kResultSystemBand moved to service_internal.hpp.
// g_run_result DEFINED here (thread_local; parallel lanes don't clobber each other).
thread_local RunResult g_run_result;

// Installed into the script DLL (xi_script_set_result_callback) so xi::result()
// records the one per-run verdict. The host is the trust boundary: a user code in
// the reserved system band is NOT accepted as-is — it's recorded as NA (0) with a
// visible warning + the offending code preserved in the message, so the mistake
// surfaces instead of masquerading as a real verdict.
void result_cb(int code, const char* msg) {
    if (code <= kResultSystemBand) {
        if (auto* srv = g_eng.srv_for_bp.load(std::memory_order_acquire)) {
            xp::LogMsg lm;
            lm.level = "warn";
            lm.msg = "xi::result(" + std::to_string(code) + ") uses a reserved system "
                     "code (<= -990000); the valid ng range is -1..-989999. Recorded as "
                     "NA (0) — fix the script's result code.";
            srv->send_text(lm.to_json());
        }
        g_run_result.code = 0;   // NA, not a fake ng1
        g_run_result.msg = "[invalid result code " + std::to_string(code) + ", reserved band] ";
        g_run_result.msg += (msg ? msg : "");
        g_run_result.set = true;
        return;
    }
    g_run_result.code = code;
    g_run_result.msg.assign(msg ? msg : "");
    g_run_result.set = true;
}

// Stable schema tag for the run_result wire event (bump on a breaking change to
// the field set). Rides as an additive "schema" field so consumers can version.
static constexpr const char* kRunResultSchema = "xi.run-outcome/1";

// Format a 128-bit trigger id as a 32-char lowercase hex string ("hi" then "lo",
// each zero-padded to 16). A null id (0/0) → empty string (omitted on the wire).
std::string trigger_id_hex(xi_trigger_id id) {   // decl in header (cross-TU)
    if (id.hi == 0 && id.lo == 0) return {};
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (int shift = 60; shift >= 0; shift -= 4) s.push_back(d[(id.hi >> shift) & 0xF]);
    for (int shift = 60; shift >= 0; shift -= 4) s.push_back(d[(id.lo >> shift) & 0xF]);
    return s;
}

// Derive the outcome class string from the EXISTING signed code — a pure read,
// it never changes the numeric code. Bands: >0 → "ok"; ==0 → "na"; the reserved
// system markers map to their own classes (dropped/crashed/no_verdict); a valid
// ng code (<0 and above the reserved system band) → "ng"; anything else in the
// system band → "na". The crash/no-verdict paths now emit their own reserved
// codes, so class and code agree even when emit_run_result derives the class.
static const char* outcome_class_for_code(int code) {
    if (code == XI_SYS_DROPPED)     return "dropped";
    if (code == XI_SYS_CRASHED)     return "crashed";
    if (code == XI_SYS_NO_VERDICT)  return "no_verdict";
    if (code > 0)                return "ok";
    if (code == 0)               return "na";
    if (code > kResultSystemBand) return "ng";   // valid ng band: <0 and > -990000
    return "na";                                  // other reserved system codes
}

// Emit a `run_result` wire event. Fields ride directly in the event data (same
// envelope shape as run_finished). Used by the inspect path (run_id >= 0) and the
// drop path (run_id < 0 → omitted; code = XI_SYS_DROPPED). ms < 0 omits "ms".
//
// Identity (all ADDITIVE, omitted when empty so the wire stays compact and
// existing consumers are unaffected): trigger_id (128-bit trigger id as hex),
// boot_id + station_id (process identity), a composite inspection_id
// "<station_id>/<boot_id>/<run_id>" (only when run_id>=0), a stable schema tag,
// the derived outcome class, an optional reason_code, and an optional
// script_generation (the monotonic version of the active loaded script DLL that
// produced the result; omitted when 0/unknown). The existing fields
// (code/msg/run_id/ms/source/group) and their numeric values are UNCHANGED.
// `cls`: if non-empty, overrides the code-derived class (used by the crash path,
// which keeps code 0 but is "crashed"). `reason_code`: optional, omitted if empty.
void emit_run_result(xi::ws::Server& srv, int code, const std::string& msg,
                            int64_t run_id, int64_t ms,
                            const std::string& source, const std::string& group,
                            const std::string& trigger_id,
                            const char* cls,
                            const char* reason_code,
                            int64_t script_generation) {
    std::string data = "{\"code\":" + std::to_string(code) + ",\"msg\":";
    xp::json_escape_into(data, msg);
    if (run_id >= 0) data += ",\"run_id\":" + std::to_string((long long)run_id);
    if (ms >= 0)     data += ",\"ms\":" + std::to_string((long long)ms);
    if (!source.empty()) { data += ",\"source\":"; xp::json_escape_into(data, source); }
    if (!group.empty())  { data += ",\"group\":";  xp::json_escape_into(data, group); }
    // --- additive identity fields ---
    if (!trigger_id.empty()) { data += ",\"trigger_id\":"; xp::json_escape_into(data, trigger_id); }
    if (!g_eng.boot_id.empty()) { data += ",\"boot_id\":"; xp::json_escape_into(data, g_eng.boot_id); }
    if (!g_eng.station_id.empty()) { data += ",\"station_id\":"; xp::json_escape_into(data, g_eng.station_id); }
    if (run_id >= 0) {
        // Composite id: "<station_id>/<boot_id>/<run_id>" (station_id may be empty).
        std::string insp = g_eng.station_id + "/" + g_eng.boot_id + "/" + std::to_string((long long)run_id);
        data += ",\"inspection_id\":"; xp::json_escape_into(data, insp);
    }
    data += ",\"schema\":"; xp::json_escape_into(data, std::string(kRunResultSchema));
    data += ",\"class\":"; xp::json_escape_into(data, std::string(cls ? cls : outcome_class_for_code(code)));
    if (reason_code && *reason_code) { data += ",\"reason_code\":"; xp::json_escape_into(data, std::string(reason_code)); }
    // script_generation: monotonic version of the active loaded script DLL that
    // produced this result. Omitted when 0/unknown (no script loaded, or the
    // drop path where no run ran). Unchanged across a failed compile.
    if (script_generation > 0) data += ",\"script_generation\":" + std::to_string((long long)script_generation);
    data += "}";
    xp::Event ev;
    ev.name = "run_result";
    ev.data_json = data;
    srv.send_text(ev.to_json());
}

// ---- Comms ------------------------------------------------------------------
// The out-of-process comms gateway + the xi::comms script API were removed: PLC
// I/O is now a plugin concern (a comm plugin owns the socket), and the BE-crash
// "go safe" case is the plugin's own sidecar process (it watches the BE and
// sends the line-safe message on death). See docs/archive/comms-gateway.md.

// Forward-declare: runs one inspection cycle (drives sinks + emits the run result).
// If run_id == 0, auto-generates one. frame_hint is passed to inspect().
// frame_path (optional) is plumbed to the script via
// `xi_script_set_run_context`; readable inside the script as
// `xi::current_frame_path()`. Empty string means none.
// run_one_inspection declared (with default args) in service_internal.hpp.

// Path resolution for the script compiler. Backend derives its own dir at
// startup and uses that to locate the xi headers we ship alongside the exe.
// Accelerator install roots, probed once at startup. Empty string =
// not installed → user scripts fall back to portable C++ for that path.
// vcvars64.bat override (empty = let the compiler auto-find via auto_find_vcvars).
// Canonical folder of the currently-open project (the one the user edits — NOT
// any .xinsp_work scratch). Set on open_project; used to read/write the
// per-project "toolchain" override block in its project.json.
// The xi include dir derived from the exe location at startup. Kept separate from
// g_eng.include_dir so a project override can point elsewhere yet we can always fall
// back to the shipped headers.

// IntelliSense config: the C/C++ extension (Microsoft) has no way to know our
// compile flags — inspect.cpp / plugin .cpp are compiled by the backend, not by
// CMake — so #include <xi/xi.hpp> and <opencv2/...> light up red. The fix (a
// c_cpp_properties.json mirroring the compiler's include set) is now written by
// the VS Code EXTENSION, which reads the resolved paths via cmd:toolchain_health.
// The core no longer touches .vscode — it only EXPOSES the paths (below).

// ---- C++ toolchain health + per-project override -----------------------------
//
// A project may pin toolchain paths in its project.json "toolchain" block:
//   "toolchain": {
//     "include_dir":     "...",   // xi headers (defaults to the shipped set)
//     "opencv_dir":      "...",   // OpenCV install root
//     "turbojpeg_root":  "...",   // libjpeg-turbo root (optional accelerator)
//     "ipp_root":        "...",   // Intel IPP root      (optional accelerator)
//     "vcvars":          "..."    // path to vcvars64.bat (else auto-found)
//   }
// Resolution priority per component: project override > env var > built-in probe
// (which itself checks env then default candidates). This lets a user fix a
// wrong/missing path from the VS Code config UI without touching global
// environment. The same resolved values feed the compiler AND (via
// cmd:toolchain_health) the extension's c_cpp_properties.json, so IntelliSense
// can't drift from the build.
//
// The subsystem itself (component probing, override read/write, health JSON) is
// an UNRELATED concern lifted into xi/xi_toolchain.hpp (design review lens 2#6).
// It's pure functions of a project folder + the injected default include dir;
// service_main only owns the *global* resolved compiler paths (below) because the
// compile path consumes them, and copies xi::toolchain::resolve()'s result in.

// Apply a project's toolchain resolution to the global compiler paths. Called on
// open_project and after set_toolchain_override so the next compile + the
// generated IntelliSense config both pick up the override immediately.
void resolve_toolchain_(const std::string& folder) {
    auto r = xi::toolchain::resolve(folder, g_eng.include_dir_default);
    g_eng.include_dir    = r.include_dir;
    g_eng.opencv_dir     = r.opencv_dir;
    g_eng.turbojpeg_root = r.turbojpeg_root;
    g_eng.ipp_root       = r.ipp_root;
    g_eng.tc_vcvars      = r.vcvars;
    std::fprintf(stderr, "[xinsp2] toolchain resolved: opencv=%s turbojpeg=%s ipp=%s vcvars=%s\n",
                 g_eng.opencv_dir.empty() ? "none" : g_eng.opencv_dir.c_str(),
                 g_eng.turbojpeg_root.empty() ? "none" : g_eng.turbojpeg_root.c_str(),
                 g_eng.ipp_root.empty() ? "none" : g_eng.ipp_root.c_str(),
                 g_eng.tc_vcvars.empty() ? "auto" : g_eng.tc_vcvars.c_str());
}

// ---- script external dependencies (project.json include_dirs / link_libs) ----
//
// A user script (inspect.cpp) may need an external SDK. Unlike a plugin (which
// ships deps in its own folder), the script DLL lives in TEMP/script_build, so we
// give the script two project-level hooks:
//   "include_dirs": ["deps/include", "C:/abs/include"]  -> extra cl /I
//   "link_libs":    ["deps/foo.lib"]                     -> import libs to link
// Relative entries resolve against the project folder. The matching runtime DLL
// search of the project folder is set up in set_project_dll_search_ (below).
//
// NOTE: there is deliberately no "sources" (extra .cpp TUs) hook for scripts.
// The script-support thunks + xi::use()/VAR/state callback globals are bound
// per-TU (xi_use.hpp's `extern` resolves to xi_script_support.hpp's `static`
// only within the force-included primary TU), so a second .cpp TU can't call
// use()/VAR. Multi-file scripts split via headers #included into the one TU —
// see docs/guides/write-a-script.md. (Plugins, which export a C ABI, use
// extra_sources; scripts can't follow that model for this reason.)
void read_script_deps_(const std::string& folder,
                              std::vector<std::string>& include_dirs,
                              std::vector<std::string>& link_libs,
                              int& openmp_max_threads) {
    if (folder.empty()) return;
    namespace fs = std::filesystem;
    std::ifstream in((fs::path(folder) / "project.json").string());
    if (!in) return;
    std::stringstream ss; ss << in.rdbuf();
    std::string src = ss.str();
    yyjson_doc* doc = yyjson_read(src.c_str(), src.size(), 0);
    if (!doc) return;
    yyjson_val* root = yyjson_doc_get_root(doc);
    auto resolve = [&](const char* s) -> std::string {
        fs::path p(s);
        if (p.is_absolute()) return p.string();
        std::error_code ec;
        return (fs::path(folder) / p).lexically_normal().string();
    };
    auto pull = [&](const char* key, std::vector<std::string>& out) {
        yyjson_val* arr = yyjson_obj_get(root, key);
        if (!arr || !yyjson_is_arr(arr)) return;
        size_t _i, _n; yyjson_val* it;
        yyjson_arr_foreach(arr, _i, _n, it) {
            const char* s = yyjson_get_str(it);
            if (yyjson_is_str(it) && s && *s)
                out.push_back(resolve(s));
        }
    };
    pull("include_dirs", include_dirs);
    pull("link_libs", link_libs);
    // Opt-in OpenMP for the script compile. 0/absent = off, N>0 = on capped to N,
    // -1 = on uncapped. Adds /openmp (+ cap macro) in the compiler.
    yyjson_val* omp = yyjson_obj_get(root, "openmp_max_threads");
    if (omp && yyjson_is_num(omp)) openmp_max_threads = yyjson_get_int(omp);
    yyjson_doc_free(doc);
}

// Put the open project's folder on the process DLL search path so a script's
// statically-linked external dependency DLL can live in the project folder. The
// script loader (xi_script_loader.hpp) loads with LOAD_LIBRARY_SEARCH_USER_DIRS,
// which honours dirs added via AddDllDirectory. Re-pointed on each open_project.
// TODO(linux): equivalent is building the script .so with -Wl,-rpath plus
// dlopen; AddDllDirectory has no portable analogue.
void set_project_dll_search_(const std::string& folder) {
    if (g_eng.proj_dll_dir) { RemoveDllDirectory(g_eng.proj_dll_dir); g_eng.proj_dll_dir = nullptr; }
    if (folder.empty()) return;
    int wn = MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), -1, nullptr, 0);
    if (wn <= 0) return;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), -1, w.data(), wn);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    g_eng.proj_dll_dir = AddDllDirectory(w.c_str());
}

// Plugin manager (global)

// (forward-declared above use_process_cb) record a per-instance process() crash.
void note_instance_crash_(const char* name, const char* why) {
    if (name) g_eng.plugin_mgr.note_instance_crash(name, why ? why : "process() crashed");
}

// (forward-declared above use_process_inline_) Part III G2.1 culprit stamp.
// The plugin name -> {folder, dll} resolution needs the manager lock, so a
// thread_local cache resolves it ONCE per (plugin, thread) and re-resolves only
// when the active plugin changes — the per-frame process() hot path then costs
// just a string compare + the four strncpy inside set_culprit().
void stamp_culprit_(const char* instance, const std::string& plugin) {
    thread_local std::string t_plugin, t_folder, t_dll;
    if (plugin != t_plugin) {
        t_plugin = plugin;
        t_folder.clear();
        t_dll.clear();
        if (!plugin.empty())
            g_eng.plugin_mgr.plugin_location(plugin, t_folder, t_dll);  // lock only on change
    }
    xi::crash::set_culprit(instance ? instance : "", plugin.c_str(),
                           t_folder.c_str(), t_dll.c_str());
}

// T2: set at the end of controlled_shutdown_teardown_ so the console-control
// handler can wait for a clean teardown before the OS force-terminates on a
// window close / logoff / shutdown (which give only a short grace window).

// CLI/env arg parsing (get_exe_dir / parse_port / parse_host / parse_watchdog_ms
// / parse_auth_secret / parse_str_flag / has_flag / parse_autostart_fps /
// parse_extra_plugin_dirs) moved to xi/xi_cli_args.hpp (namespace xi::cli).

double now_seconds() { return xi::wall_us() / 1e6; }   // wall: pong ts

void send_rsp_ok(xi::ws::Server& srv, int64_t id, std::string data_json) {
    xp::Rsp r;
    r.id = id;
    r.ok = true;
    r.data_json = std::move(data_json);
    srv.send_text(r.to_json());
}

// Ring buffer of recent error messages so an AI / scripted client can
// correlate a synchronous cmd with any side-channel errors that might
// have raced in (run-thread crashes, log-level=error from background
// activity, etc). Three error channels exist
// in the protocol — rsp.error (sync), `event` (async), `log`
// level=error (async) — and the WS spec doesn't carry cmd_id /
// run_id on the async two. Until that's fixed protocol-wide, this
// ring lets the client pull "anything error-shaped that happened
// in the last minute" with a single query.
// kRecentErrorsCap moved to service_internal.hpp.
int64_t now_ms_() { return xi::wall_ms(); }   // wall: RecentError ts

void push_recent_error(std::string source, std::string message,
                              int64_t cmd_id, int64_t run_id) {
    RecentError e{ now_ms_(), std::move(source), std::move(message), cmd_id, run_id };
    std::lock_guard<std::mutex> lk(g_eng.recent_errors_mu);
    g_eng.recent_errors.push_back(std::move(e));
    while (g_eng.recent_errors.size() > kRecentErrorsCap) g_eng.recent_errors.pop_front();
}

void send_rsp_err(xi::ws::Server& srv, int64_t id, std::string err) {
    xp::Rsp r;
    r.id = id;
    r.ok = false;
    r.error = err;
    srv.send_text(r.to_json());
    push_recent_error("rsp", std::move(err), id);
}

// Send a log {level:error, msg:...} AND record it in the recent-error
// ring so cmd:recent_errors can surface it. Most error logs go
// through this; a few legacy sites still build the LogMsg inline —
// migrating them to this helper is mechanical and ongoing.
void emit_error_log(xi::ws::Server& srv, const std::string& msg,
                           int64_t run_id) {
    xp::LogMsg lm; lm.level = "error"; lm.msg = msg;
    srv.send_text(lm.to_json());
    push_recent_error("log", msg, /*cmd_id=*/0, run_id);
}

void send_hello(xi::ws::Server& srv) {
    xp::Event e;
    e.name = "hello";
    e.data_json = std::string(R"({"version":")") + XINSP2_VERSION
                + R"(","commit":")" + XINSP2_COMMIT
                + R"(","abi":1})";
    srv.send_text(e.to_json());
}


// (seh_exception and seh_translator defined above, before use_process_cb)

// ---- single-frame inspection cycle extracted to service_inspect.cpp ---------

// (trigger_worker removed — continuous mode uses a simple timer thread)

// ---- Dispatch groups / pool lifecycle / trigger sink / teardown / instance-state
//      extracted to service_dispatch.cpp ------------------------------------

// ============================================================================
// WS command handlers  (TASTE refactor, lens 2#2): the former handle_command
// if/else-if god-chain, split into one named handler per command + a dispatch
// table (g_cmd_table).  Each handler body is byte-identical to the arm it came
// from; only the control-flow wrapper changed.  Grouped by capability below.
// Uniform signature: (xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed)
// -- bodies reference srv / id / parsed->args_json exactly as the arms did.
// ============================================================================

// ---- lifecycle handlers extracted to service_cmd_lifecycle.cpp -------------
// ---- dispatch-control handlers extracted to service_cmd_dispatch.cpp --------
// ---- observability handlers extracted to service_cmd_observability.cpp ------
// ---- project-CRUD handlers extracted to service_cmd_project.cpp ------------
// ---- plugin-mgmt handlers extracted to service_cmd_plugin.cpp --------------

// Dispatch table: command name -> handler.  Replaces the if/else-if chain.
using HandlerFn = void(*)(xi::ws::Server&, int64_t, const xp::ParsedCmd*);
static const std::unordered_map<std::string_view, HandlerFn> g_cmd_table = {
    {"ping", cmd_ping_},
    {"version", cmd_version_},
    {"crash_reports", cmd_crash_reports_},
    {"clear_crash_reports", cmd_clear_crash_reports_},
    {"set_watchdog_ms", cmd_set_watchdog_ms_},
    {"set_process_priority", cmd_set_process_priority_},
    {"set_timer_fps", cmd_set_timer_fps_},
    {"watchdog_status", cmd_watchdog_status_},
    {"graph_capture", cmd_graph_capture_},
    {"graph_snapshot", cmd_graph_snapshot_},
    {"shutdown", cmd_shutdown_},
    {"compile_and_load", cmd_compile_and_load_},
    {"unload_script", cmd_unload_script_},
    {"run", cmd_run_},
    {"start", cmd_start_},
    {"stop", cmd_stop_},
    {"list_params", cmd_list_params_},
    {"set_param", cmd_set_param_},
    {"list_instances", cmd_list_instances_},
    {"set_instance_def", cmd_set_instance_def_},
    {"get_instance_def", cmd_get_instance_def_},
    {"exchange_instance", cmd_exchange_instance_},
    {"get_state", cmd_get_state_},
    {"prepare_instance", cmd_prepare_instance_},
    {"commit_group", cmd_commit_group_},
    {"save_project", cmd_save_project_},
    {"commit_working_copy", cmd_commit_working_copy_},
    {"discard_working_copy", cmd_discard_working_copy_},
    {"load_project", cmd_load_project_},
    {"list_plugins", cmd_list_plugins_},
    {"recent_errors", cmd_recent_errors_},
    {"status", cmd_status_},
    {"image_pool_stats", cmd_image_pool_stats_},
    {"rescan_plugins", cmd_rescan_plugins_},
    {"unquarantine_plugin", cmd_unquarantine_plugin_},
    {"load_plugin", cmd_load_plugin_},
    {"create_project", cmd_create_project_},
    {"open_project", cmd_open_project_},
    {"close_project", cmd_close_project_},
    {"export_project_plugin", cmd_export_project_plugin_},
    {"recompile_project_plugin", cmd_recompile_project_plugin_},
    {"rebuild_plugins", cmd_rebuild_plugins_},
    {"dispatch_stats", cmd_dispatch_stats_},
    {"metrics", cmd_metrics_},
    {"open_project_warnings", cmd_open_project_warnings_},
    {"create_instance", cmd_create_instance_},
    {"remove_instance", cmd_remove_instance_},
    {"rename_instance", cmd_rename_instance_},
    {"get_project", cmd_get_project_},
    {"save_instance_config", cmd_save_instance_config_},
    {"get_plugin_ui", cmd_get_plugin_ui_},
    {"get_dashboard", cmd_get_dashboard_},
    {"toolchain_health", cmd_toolchain_health_},
    {"set_toolchain_override", cmd_set_toolchain_override_},
};

static void handle_command(xi::ws::Server& srv, std::string_view text) {
    auto parsed = xp::parse_cmd(text);
    if (!parsed) {
        xp::LogMsg lm;
        lm.level = "error";
        lm.msg   = std::string("malformed cmd: ") + std::string(text.substr(0, 128));
        srv.send_text(lm.to_json());
        return;
    }

    const auto& name = parsed->name;
    const int64_t id = parsed->id;

    auto it = g_cmd_table.find(name);
    if (it != g_cmd_table.end()) {
        it->second(srv, id, &*parsed);
    } else {
        send_rsp_err(srv, id, std::string("unknown command: ") + name);
    }
}


// Generate the per-process boot_id (random 128-bit → 32-char lowercase hex) and
// read the optional station_id from XINSP_STATION_ID. Runs ONCE at startup (not
// per-frame), so std::random_device + a seeded 64-bit engine is fine here. Both
// land in g_eng and are read-only afterward, so every run_result emission can
// read them lock-free.
static void init_process_identity_() {
    std::random_device rd;
    std::seed_seq seed{ rd(), rd(), rd(), rd(),
                        (unsigned)std::chrono::steady_clock::now().time_since_epoch().count() };
    std::mt19937_64 eng(seed);
    uint64_t hi = eng(), lo = eng();
    if (hi == 0 && lo == 0) lo = 1;   // never all-zero (would read as "null")
    g_eng.boot_id = trigger_id_hex(xi_trigger_id{ hi, lo });

    if (const char* s = std::getenv("XINSP_STATION_ID"); s && *s)
        g_eng.station_id = s;
}

int main(int argc, char** argv) {
    // Part III G1.1 — certify mode: load a plugin DLL + call its factory once in
    // THIS throwaway child process, exit with a verdict code (0 ok / 42
    // abi_mismatch / abnormal = crashed). Crash-isolation for discovery: a
    // malformed DLL faults HERE, never in the scanning backend. Handled BEFORE
    // install_seh_translator() so a fault reaches the minidump filter rather than
    // being translated to a catchable exception + swallowed.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--certify-plugin") {
            const char* dir = (i + 1 < argc) ? argv[i + 1] : nullptr;
            xi::crash::install();   // a crashed certify still yields a minidump
            int code = dir ? xi::certify::certify_in_process(dir)
                           : xi::certify::kExitAbiMismatch;
            std::fflush(stderr);
            std::fflush(stdout);
            return code;
        }
    }

    // Install the crash-forensics handlers (minidump filter + CRT death-path
    // interceptors + first-chance logger + fault-stack reserve). Lives in the
    // extracted leaf xi_crash_dump.hpp.
    xi::crash::install();
    // SEH → C++ exception translator so try/catch catches segfaults (a separate
    // concern from the dump machinery; owned here, re-set per inspect thread).
    xi::install_seh_translator();

    // --test-crash: deliberately trigger a fatal exception so the
    // top-level minidump filter fires. Used by runCrashDump E2E.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--test-crash") {
            RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
            return 99;  // unreachable
        }
        // --test-abort: exercise the CRT abort() path (robustness BUG 1) — must
        // produce the same minidump + .json sidecar as a real exception crash.
        if (std::string_view(argv[i]) == "--test-abort") {
            std::abort();
            return 99;  // unreachable
        }
        // --test-stackoverflow: exercise the stack-overflow path (robustness BUG 2)
        // — with reserve_fault_stack() the filter must still write a dump+sidecar.
        if (std::string_view(argv[i]) == "--test-stackoverflow") {
            reserve_fault_stack();
            // Unbounded recursion with a volatile sink the optimizer can't elide.
            struct Boom { static int go(volatile int x) { return x + go(x + 1) + go(x + 2); } };
            return Boom::go(1);  // unreachable — overflows the stack
        }
    }

    // --version / -v / --help / -h short-circuit before any side effects.
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--version" || a == "-v") {
            std::printf("xinsp-backend %s (%s)\n", XINSP2_VERSION, XINSP2_COMMIT);
            return 0;
        }
        if (a == "--help" || a == "-h") {
            std::printf(
                "xinsp-backend %s — xInsp2 inspection server\n"
                "\n"
                "Usage: xinsp-backend [options]\n"
                "  --port=N             WebSocket port (default 7823)\n"
                "  --host=ADDR          bind address (default 127.0.0.1; use 0.0.0.0 for remote)\n"
                "  --auth=SECRET        require Bearer SECRET in handshake\n"
                "  --plugins-dir=DIR    extra plugin folder (repeatable)\n"
                "  --watchdog=MS        per-inspect budget: cooperative-cancel, then exit\n"
                "                       for FE respawn if ignored (default 0 = off)\n"
                "  --project=DIR        headless autostart: open this project at boot\n"
                "  --script=PATH        script to compile for --project (default: project.json's)\n"
                "  --autostart-fps=N    with --project, start continuous mode at N fps (0 = off)\n"
                "  --working-copy       edit a <project>/.xinsp_work scratch copy (transactional;\n"
                "                       resumes on crash respawn). commit_working_copy to save\n"
                "  --priority=CLASS     process priority: high|above|normal|below|realtime (Win)\n"
                "  --aot                prebuilt bundle: load existing plugin/script DLLs, no compiler\n"
                "  --version, -v        print version and exit\n"
                "  --help, -h           this help\n",
                XINSP2_VERSION);
            return 0;
        }
    }

    int port = xi::cli::parse_port(argc, argv);

    // Per-process identity for run_result (boot_id + optional station_id). Once,
    // early, before any inspection can emit. See init_process_identity_.
    init_process_identity_();

    // ---- thread/process performance knobs --------------------------------------
#ifdef _WIN32
    // Raise the OS timer resolution to 1ms (default ~15.6ms) so timer-tick fps,
    // sleeps, and CV waits are tight. winmm.timeBeginPeriod via runtime-load so we
    // don't add a link dependency. Process-wide; the paired timeEndPeriod is optional.
    if (HMODULE w = LoadLibraryA("winmm.dll")) {
        if (auto fn = (UINT(WINAPI*)(UINT))GetProcAddress(w, "timeBeginPeriod")) fn(1);
    }
    // --priority=<class>: bump the whole backend's process priority (for a
    // dedicated inspection PC). Default = leave as-is. Also settable live via
    // cmd:set_process_priority / project.json runtime.process_priority.
    if (std::string pri = xi::cli::parse_str_flag(argc, argv, "--priority"); !pri.empty()) {
        if (!apply_process_priority_(pri))
            std::fprintf(stderr,
                "[xinsp2] unknown --priority '%s' (high|above|normal|below|realtime)\n", pri.c_str());
    }
#else
    // TODO(linux): clock_nanosleep is already high-res; setpriority(PRIO_PROCESS)
    // / sched_setscheduler for --priority.
#endif

    // Derive include dir for the script compiler. In a normal dev tree the
    // backend .exe is at backend/build/Release, and headers are at
    // backend/include. Walk up until we find xi/xi.hpp.
    {
        std::filesystem::path p = xi::cli::get_exe_dir();
        for (int i = 0; i < 6; ++i) {
            if (std::filesystem::exists(p / "include" / "xi" / "xi.hpp")) {
                g_eng.include_dir = (p / "include").string();
                break;
            }
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
        if (g_eng.include_dir.empty()) {
            // Fallback: next to the exe.
            g_eng.include_dir = (std::filesystem::path(xi::cli::get_exe_dir()) / "include").string();
        }
        // Remember the shipped headers as the default a project override falls
        // back to (see resolve_toolchain_).
        g_eng.include_dir_default = g_eng.include_dir;
    }
    g_eng.work_dir = (std::filesystem::temp_directory_path() / "xinsp2").string();
    std::filesystem::create_directories(g_eng.work_dir);

    // Probe accelerators once. Logged so the user can see what their
    // compiled scripts will inherit.
    g_eng.opencv_dir     = xi::script::detail::probe_opencv_dir();
    g_eng.turbojpeg_root = xi::script::detail::probe_turbojpeg_root();
    g_eng.ipp_root       = xi::script::detail::probe_ipp_root();
    std::fprintf(stderr, "[xinsp2] script-side accelerators: opencv=%s  turbojpeg=%s  ipp=%s\n",
                 g_eng.opencv_dir.empty()     ? "no" : g_eng.opencv_dir.c_str(),
                 g_eng.turbojpeg_root.empty() ? "no" : g_eng.turbojpeg_root.c_str(),
                 g_eng.ipp_root.empty()       ? "no" : g_eng.ipp_root.c_str());

    // Find and scan plugins directory (sibling of backend/)
    {
        std::filesystem::path p = xi::cli::get_exe_dir();
        for (int i = 0; i < 6; ++i) {
            if (std::filesystem::exists(p / "plugins")) {
                g_eng.plugins_dir = (p / "plugins").string();
                break;
            }
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
    }
    // G1.3 — certify each discovered plugin in a throwaway child (this same
    // backend exe, --certify-plugin mode) before arming it during the scan. A DLL
    // that crashes certification is skipped + surfaced (g_eng.plugin_mgr.certify_
    // warnings()), so discovery can never load a known-bad DLL into the backend.
    {
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (n) g_eng.plugin_mgr.set_certify_exe(std::string(exe, n));
    }
    if (!g_eng.plugins_dir.empty()) {
        int n = g_eng.plugin_mgr.scan_plugins(g_eng.plugins_dir);
        std::fprintf(stderr, "[xinsp2] scanned %d plugins from %s\n", n, g_eng.plugins_dir.c_str());
    }
    // Additional plugin folders from --plugins-dir / XINSP2_EXTRA_PLUGIN_DIRS.
    // Lets external SDKs keep their plugin DLLs in place — no copy needed.
    for (auto& dir : xi::cli::parse_extra_plugin_dirs(argc, argv)) {
        if (!std::filesystem::exists(dir)) {
            std::fprintf(stderr, "[xinsp2] extra plugin dir not found: %s\n", dir.c_str());
            continue;
        }
        int n = g_eng.plugin_mgr.scan_plugins(dir);
        std::fprintf(stderr, "[xinsp2] scanned %d plugins from %s\n", n, dir.c_str());
    }

    // G1.3 / G2.2 — surface any plugin gated out at discovery (certify crashed, or
    // FE-quarantined) into the recent-errors ring so cmd:recent_errors + the
    // extension toast tell the operator WHICH plugin is disabled and why (not just
    // a silently-missing plugin). The scan already logged each to stderr.
    for (auto& w : g_eng.plugin_mgr.certify_warnings())
        push_recent_error("plugin", w.reason);

    std::fprintf(stderr, "[xinsp2] include_dir=%s\n", g_eng.include_dir.c_str());
    std::fprintf(stderr, "[xinsp2] work_dir=%s\n",    g_eng.work_dir.c_str());
    std::fprintf(stderr, "[xinsp2] plugins_dir=%s\n",  g_eng.plugins_dir.c_str());

    // Process isolation + SHM removed 2026-05: all plugins run
    // in-process and share the host ImagePool directly (zero-copy via
    // pointers, no cross-process marshalling). No worker process, no
    // shared-memory region to set up.

    // Hand the same compile environment that xi::script::compile uses
    // to the plugin manager — project plugins (compiled when a project
    // is opened) need the include dir, vcvars, and accelerator roots.
    xi::CompileEnv env;
    env.include_dir    = g_eng.include_dir;
    env.opencv_dir     = g_eng.opencv_dir;
    env.turbojpeg_root = g_eng.turbojpeg_root;
    env.ipp_root       = g_eng.ipp_root;
    // --aot: this is a prebuilt bundle — load existing plugin DLLs instead of
    // compiling (no cl.exe on the target). The autostart script should point at a
    // .dll too (compile_and_load loads a .dll directly).
    env.aot = xi::cli::has_flag(argc, argv, "--aot");
    if (env.aot) std::fprintf(stderr, "[xinsp2] AOT mode: loading prebuilt plugin/script DLLs (no compiler)\n");
    g_eng.plugin_mgr.set_compile_env(env);

    xi::ws::Server srv;
    srv.on_open  = [&] {
        std::fprintf(stderr, "[xinsp2] client connected\n");
        send_hello(srv);
    };
    srv.on_close = [&] {
        std::fprintf(stderr, "[xinsp2] client disconnected\n");
        // E-P1-2: a fresh client should get a fresh server view. Without this
        // clear the next driver to reconnect inherits the prior session's error
        // ring — not visible from the new client's perspective, at best confuses,
        // at worst hides regressions.
        {
            std::lock_guard<std::mutex> lk(g_eng.recent_errors_mu);
            g_eng.recent_errors.clear();
        }
        // (E-P1-1: the per-instance-death dedup set was removed with
        // process isolation in 2026-05; no set to clear here.)
    };
    srv.on_text = [&](std::string_view s) {
        handle_command(srv, s);
    };
    srv.on_binary = [&](const uint8_t*, size_t n) {
        std::fprintf(stderr, "[xinsp2] unexpected binary frame: %zu bytes\n", n);
    };

    std::string host   = xi::cli::parse_host(argc, argv);
    std::string secret = xi::cli::parse_auth_secret(argc, argv);
    srv.set_bind_host(host);
    if (!secret.empty()) srv.set_auth_secret(secret);

    g_eng.watchdog_ms = xi::cli::parse_watchdog_ms(argc, argv);
    if (g_eng.watchdog_ms.load() > 0) {
        std::fprintf(stderr, "[xinsp2] watchdog enabled: %d ms per inspect\n", g_eng.watchdog_ms.load());
    }
    g_eng.srv_for_bp = &srv;   // status_cb + dropped-frame markers emit through it
    // Route plugin host_api->set_status into the status registry. Non-capturing
    // so it converts to the StatusSinkFn function pointer.
    xi::status_sink() = [](const char* who, const char* text) {
        set_status_internal((who && *who) ? who : "@plugin", text);
    };
    // ABI v8: route plugin host_api->emit_binary straight to WS clients. The core
    // is a dumb byte pipe — the frame format is the plugin's contract with its UI.
    // Non-capturing → converts to the BinarySinkFn function pointer. Thread-safe:
    // send_binary may be called from a dispatch worker (any sink's binary push path).
    xi::binary_sink() = [](const void* data, int len) {
        if (auto* s = g_eng.srv_for_bp.load(std::memory_order_acquire))
            s->send_binary(static_cast<const uint8_t*>(data), static_cast<size_t>(len));
    };
    // ABI v9: a generic JPEG-encode host service — process-global N-rotate cache
    // keyed by a content hash of the pixels, so the SAME image compressed by
    // several plugins (or repeatedly) is encoded ONCE globally. Plugin-agnostic
    // convenience. Non-capturing → converts to CompressSinkFn.
    xi::compress_sink() = [](const void* px, int w, int h, int c, int q,
                             void* out, int cap) -> int {
        if (!px || w <= 0 || h <= 0 || c <= 0) return 0;
        const size_t nbytes = (size_t)w * (size_t)h * (size_t)c;
        uint64_t key = 1469598103934665603ull;          // FNV-1a over the pixels...
        const uint8_t* p = static_cast<const uint8_t*>(px);
        for (size_t i = 0; i < nbytes; ++i) { key ^= p[i]; key *= 1099511628211ull; }
        key ^= ((uint64_t)w << 40) ^ ((uint64_t)h << 16) ^ (uint64_t)(c * 1000 + q);  // ...+ dims/quality
        constexpr size_t kCap = 32;
        static std::mutex cmu;
        static std::unordered_map<uint64_t, std::vector<uint8_t>> cache;
        static std::deque<uint64_t> order;
        std::vector<uint8_t> jpeg;
        {
            std::lock_guard<std::mutex> lk(cmu);
            auto it = cache.find(key);
            if (it != cache.end()) {
                jpeg = it->second;                       // cache hit → reuse the encode
            } else {
                xi::Image img = xi::Image::view(w, h, c, const_cast<uint8_t*>(p));
                if (!xi::encode_jpeg(img, q, jpeg) || jpeg.empty()) return 0;
                order.push_back(key);
                while (order.size() > kCap) { cache.erase(order.front()); order.pop_front(); }
                cache.emplace(key, jpeg);
            }
        }
        if ((int)jpeg.size() > cap) return -(int)jpeg.size();
        std::memcpy(out, jpeg.data(), jpeg.size());
        return (int)jpeg.size();
    };

    // P1-3: forward plugin/script host_api->log to the operator channel. stderr is
    // unwatched on an unattended PC, so a plugin's WARN/ERROR self-diagnostics used
    // to vanish. WARN/ERROR escalate to the WS log channel (clients see them live +
    // re-pull via cmd:recent_errors); ERROR also lands in the recent-errors ring.
    // DEBUG/INFO stay stderr-only (already printed in make_host_api) to avoid
    // flooding the WS log. Non-capturing → converts to LogSinkFn. Thread-safe:
    // send_text + push_recent_error may be called from any dispatch worker.
    xi::log_sink() = [](int32_t level, const char* msg, int64_t /*ts_ms*/) {
        if (level < 2 || !msg) return;                 // only WARN(2)/ERROR(3) escalate
        if (auto* s = g_eng.srv_for_bp.load(std::memory_order_acquire)) {
            xp::LogMsg lm; lm.level = (level >= 3) ? "error" : "warn"; lm.msg = msg;
            s->send_text(lm.to_json());
        }
        if (level >= 3) push_recent_error("plugin", msg);
    };

    // P2.4 watchdog. Always-on monitor thread; acts when any in-flight inspect
    // (wd_arm slot) overruns its deadline. Two-phase, now per-worker-aware:
    //   Phase 1 — cooperative: arm the script's EPOCH-scoped cancel
    //     (set_global_cancel(1)); xi::ops poll xi::cancellation_requested() and
    //     bail. 1000 ms grace (big ops — 20 MP gaussian, matchTemplate, contour
    //     walks — need a few hundred ms to finish their current chunk; 100 ms
    //     tripped healthy scripts). The arm targets only inspects ALREADY in
    //     flight at trip time (ticket below the high-water snapshot): under N>1
    //     it aborts every currently-running frame this round — the intended
    //     "something's wedged, bail" signal — but a FRESH frame the pool starts
    //     during the grace draws a higher ticket and is NOT cancelled, so one
    //     slow frame no longer poisons ~a second of unrelated frames. Healthy
    //     workers re-run next tick. (Pre-fix the flag was a held global bool, so
    //     every heavy frame dispatched in the grace window aborted spuriously —
    //     core-bug-hunt 2026-06 #12.)
    //   Phase 2 — hard trip: if any slot is STILL overrun after the grace, the
    //     script ignored cooperative cancel. We do NOT TerminateThread — a kill
    //     mid process() would leak the per-instance lock (deadlocking that
    //     instance) and risk heap corruption. The process is unrecoverable, so
    //     the backend EXITS; the FE supervisor respawns a clean one (and drives
    //     the line safe). Run without an FE => backend stays down by design.
    g_eng.watchdog_run = true;
    g_eng.watchdog_thread = std::thread([&srv]() {
        auto now_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        // Snapshot of the slot deadlines that were overrun when we armed a
        // cooperative cancel. After the grace we hard-trip ONLY if one of THESE
        // same inspects is still stuck (same slot still holds the same deadline)
        // — i.e. it ignored the cooperative cancel it was actually targeted by.
        // A different inspect overrunning by then (a fresh frame that started
        // during the grace, which the epoch-scoped cancel deliberately did NOT
        // target) is left for the next loop iteration to give its OWN
        // cooperative round, rather than being hard-killed without warning.
        int64_t wd_snap[WD_SLOTS];
        while (g_eng.watchdog_run.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            int64_t now = now_ms();
            bool any_overran = false;
            for (int i = 0; i < WD_SLOTS; ++i) {
                int64_t dl = g_eng.wd_deadlines[i].load();
                if (dl != 0 && now >= dl) { wd_snap[i] = dl; any_overran = true; }
                else                       { wd_snap[i] = 0; }
            }
            if (!any_overran) continue;

            // Phase 1: cooperative cancel + grace. Log the attempt so the
            // escalation is observable (and a hard trip can be proven to have
            // tried the soft cancel first, not jumped straight to the kill).
            std::fprintf(stderr,
                "[xinsp2] watchdog: inspect overran %dms — requesting cooperative cancel\n",
                g_eng.watchdog_ms.load());
            {
                std::lock_guard<std::mutex> lk(g_eng.script_mu);
                if (g_eng.script.set_global_cancel) g_eng.script.set_global_cancel(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            // Did every inspect we TARGETED return? (Its slot is now free or
            // re-armed by a different inspect with a different deadline.) Match
            // on slot index AND deadline value so a fresh inspect reusing the
            // slot is not mistaken for the original stuck one.
            bool still_stuck = false;
            for (int i = 0; i < WD_SLOTS; ++i) {
                if (wd_snap[i] != 0 && g_eng.wd_deadlines[i].load() == wd_snap[i]) {
                    still_stuck = true; break;
                }
            }
            if (!still_stuck) {
                {
                    std::lock_guard<std::mutex> lk(g_eng.script_mu);
                    if (g_eng.script.set_global_cancel) g_eng.script.set_global_cancel(0);
                }
                int n = ++g_eng.watchdog_trips;
                std::fprintf(stderr,
                    "[xinsp2] watchdog tripped (#%d) — script honoured cooperative cancel\n", n);
                emit_error_log(srv,
                    "watchdog tripped — inspect exceeded "
                    + std::to_string(g_eng.watchdog_ms.load())
                    + "ms; cooperative cancel succeeded");
                continue;
            }

            // Phase 2: hard trip — exit for FE respawn (see header above).
            ++g_eng.watchdog_trips;
            std::fprintf(stderr,
                "[xinsp2] watchdog HARD trip - inspect exceeded %dms and ignored "
                "cooperative cancel; exiting for supervisor respawn (rc=0x%04X)\n",
                g_eng.watchdog_ms.load(), WATCHDOG_EXIT_CODE);
            emit_error_log(srv,
                "watchdog HARD trip — inspect exceeded "
                + std::to_string(g_eng.watchdog_ms.load())
                + "ms and ignored cooperative cancel; backend exiting for respawn");
            std::fflush(stderr);
            std::fflush(stdout);
            // _Exit: skip static destructors / atexit — a wedged worker may hold
            // locks those would block on. The FE sees the exit and respawns.
            std::_Exit(WATCHDOG_EXIT_CODE);
        }
    });
    // Stop+join the always-on watchdog on ANY exit from here down — the bind-fail
    // `return 1` just below and the debug `--hang-*` `return 0`s all sit AFTER the
    // thread was spawned. A joinable std::thread destroyed at static teardown (the
    // file-scope g_eng.watchdog_thread) invokes std::terminate, which the still-armed
    // crash filter turns into a spurious minidump + abnormal exit — the FE reads a
    // routine port-in-use as a backend CRASH. The normal epilogue joins too; this
    // then no-ops (joinable()==false).
    struct WatchdogJoiner {
        ~WatchdogJoiner() {
            g_eng.watchdog_run = false;
            if (g_eng.watchdog_thread.joinable()) g_eng.watchdog_thread.join();
        }
    } wd_joiner_;
    if (!srv.start(port)) {
        std::fprintf(stderr, "[xinsp2] failed to bind %s:%d\n", host.c_str(), port);
        return 1;
    }
    if (host == "0.0.0.0" && secret.empty()) {
        std::fprintf(stderr,
            "[xinsp2] WARNING: bound to 0.0.0.0 with NO --auth secret — anyone reachable can drive the backend\n");
    }
    std::fprintf(stderr, "[xinsp2] listening on ws://%s:%d%s\n",
                 host.c_str(), port,
                 secret.empty() ? "" : " (auth required)");
    std::fflush(stderr);

    // Headless autostart (--project). Drives the same WS command handlers a
    // client would call — open_project, then compile_and_load, then (optional)
    // start — by synthesizing wire-format frames and feeding handle_command.
    // No client need ever connect: reply sends are no-ops while srv has no
    // client (xi_ws_server send_frame returns false at INVALID_SOCK). The WS
    // port stays open so an operator HMI / the VS Code extension can attach
    // live. Used by xinsp-fe.exe to run a line headlessly.
    if (std::string project = xi::cli::parse_str_flag(argc, argv, "--project"); !project.empty()) {
        std::string script = xi::cli::parse_str_flag(argc, argv, "--script");
        int autostart_fps  = xi::cli::parse_autostart_fps(argc, argv);
        // --working-copy: open via a <project>/.xinsp_work scratch. On a crash
        // respawn the FE passes the same flag; the scratch still exists, so the
        // backend resumes the last in-progress settings instead of reverting to
        // the pristine project. See docs/guides/deploy.md.
        bool working_copy = xi::cli::has_flag(argc, argv, "--working-copy");

        bool degraded = false;
        // Validate the project BEFORE opening. A nonexistent / project.json-less
        // dir can't inspect, so don't let it reach "ready" and be reported
        // healthy (an operator typo would otherwise yield a green, blind line).
        std::error_code proj_ec;
        if (!std::filesystem::exists(std::filesystem::path(project) / "project.json", proj_ec)) {
            std::fprintf(stderr, "[xinsp2] autostart: degraded - no project.json at %s; "
                         "cannot run (not reporting ready)\n", project.c_str());
            degraded = true;
        } else {
            std::fprintf(stderr, "[xinsp2] autostart: open_project %s%s\n", project.c_str(),
                         working_copy ? " (working copy)" : "");
            handle_command(srv,
                "{\"type\":\"cmd\",\"id\":1,\"name\":\"open_project\",\"args\":{\"path\":"
                + xp::json_escape(project)
                + (working_copy ? ",\"working_copy\":true" : "") + "}}");

            // Resolve the script path: an explicit --script relative path is
            // relative to the PROJECT dir; otherwise project.json's script_path.
            // open_project ALWAYS populates script_path (falls back to
            // "<folder>/inspect.cpp"), so a script-less project resolves to a
            // path that may not exist — treat a missing script FILE as open-only
            // rather than firing a doomed compile.
            if (!script.empty()) {
                std::filesystem::path sp(script);
                if (sp.is_relative()) sp = std::filesystem::path(project) / sp;
                script = sp.string();
            } else {
                script = g_eng.plugin_mgr.project().script_path;
            }
            if (script.empty() || !std::filesystem::exists(script)) {
                std::fprintf(stderr,
                    "[xinsp2] autostart: no script file (%s); open only\n",
                    script.empty() ? "none given" : script.c_str());
            } else {
                std::fprintf(stderr, "[xinsp2] autostart: compile_and_load %s\n", script.c_str());
                // Production/headless boot wants the OPTIMIZED (/O2) build, not the
                // interactive /Od fast-dev default.
                handle_command(srv,
                    "{\"type\":\"cmd\",\"id\":2,\"name\":\"compile_and_load\",\"args\":{\"path\":"
                    + xp::json_escape(script) + ",\"optimize\":true}}");

                // Confirm the script actually loaded. A failed compile leaves the
                // port up (operator can attach + fix) but the line CANNOT inspect,
                // so mark degraded and withhold "ready" — the FE then treats a
                // non-inspecting line as unhealthy instead of silently "healthy".
                if (!g_eng.script.ok()) {
                    degraded = true;
                    std::fprintf(stderr,
                        "[xinsp2] autostart: degraded - script failed to compile/load; "
                        "line will NOT inspect (port stays up for an operator to recompile)\n");
                } else if (autostart_fps != 0) {
                    // >0 = timer at N fps; <0 = trigger-only (no timer). The fps
                    // value passes through to cmd:start, which treats <=0 as
                    // trigger-only. (Keep the ">0" wording as "start N fps" — qa_func
                    // FE-E9 pins that line.)
                    if (autostart_fps > 0)
                        std::fprintf(stderr, "[xinsp2] autostart: start %d fps\n", autostart_fps);
                    else
                        std::fprintf(stderr, "[xinsp2] autostart: start (trigger-only, no timer)\n");
                    handle_command(srv,
                        "{\"type\":\"cmd\",\"id\":3,\"name\":\"start\",\"args\":{\"fps\":"
                        + std::to_string(autostart_fps) + "}}");
                }
            }
        }
        // Debug hook (test-only): simulate a backend that hangs DURING boot —
        // alive, port bound, but never reaches "ready". Used by the FE
        // boot-timeout test (examples/qa_race/driver_boot_hang.py). Placed
        // before the readiness marker so the FE's boot gate trips.
        if (xi::cli::has_flag(argc, argv, "--hang-before-ready")) {
            std::fprintf(stderr, "[xinsp2] autostart: --hang-before-ready (debug) — "
                                 "hanging before ready\n");
            std::fflush(stderr);
            while (!g_eng.should_exit.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            return 0;
        }

        // Readiness marker. The WS port binds in srv.start() ABOVE, but the
        // open/compile/start sequence runs synchronously here before the accept
        // loop — so for several seconds the port is "up" (TCP connect succeeds)
        // while the backend isn't yet serving. A supervisor / operator HMI / a
        // test can wait for THIS line to know the line is actually ready, not
        // merely listening. (port-up != ready.)
        // Only signal "ready" when the line can actually inspect. A degraded
        // (compile-failed) boot deliberately withholds it — the FE then sees no
        // health signal and drives the line safe rather than trusting port-up.
        if (!degraded) std::fprintf(stderr, "[xinsp2] autostart: ready\n");
        std::fflush(stderr);
    }

    // Liveness heartbeat: a monotonic counter written from the SERVING loop.
    // If a synchronous WS handler wedges srv.poll(), the counter stops advancing
    // while the port still accepts TCP — a "bound but not serving" state a
    // connect probe can't see. The FE watches this file (--heartbeat-file) and
    // respawns on staleness. No-op when the flag is unset.
    std::string hb_path = xi::cli::parse_str_flag(argc, argv, "--heartbeat-file");
    uint64_t hb_counter = 0;
    auto write_heartbeat = [&] {
        if (hb_path.empty()) return;
        if (FILE* f = std::fopen(hb_path.c_str(), "wb")) {
            std::fprintf(f, "%llu", (unsigned long long)++hb_counter);
            std::fclose(f);
        }
    };
    write_heartbeat();   // initial beat so the FE sees liveness promptly

    // Debug hook (test-only): wedge the serving loop AFTER ready — port stays
    // bound + accepting but the heartbeat goes stale, so the FE detects a
    // serve-time wedge. Drives examples/qa_race/driver_serve_wedge.py.
    if (xi::cli::has_flag(argc, argv, "--hang-after-ready")) {
        std::fprintf(stderr, "[xinsp2] --hang-after-ready (debug) — wedging the serving loop\n");
        std::fflush(stderr);
        while (!g_eng.should_exit.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return 0;
    }

    // T2: catch abrupt console exits (window close / Ctrl+C / logoff / shutdown)
    // so they run the controlled teardown instead of a bare ExitProcess. Installed
    // here — after all startup is done and the teardown machinery (srv, pool,
    // project) is live — so the handler's flip-and-wait always has a real loop to
    // service it.
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(console_ctrl_handler_, TRUE))
        std::fprintf(stderr, "[xinsp2] warning: SetConsoleCtrlHandler failed (%lu)\n",
                     (unsigned long)GetLastError());
#endif

    int64_t hb_last_ms = 0;
    while (!g_eng.should_exit.load() && srv.is_running()) {
        srv.poll(100);
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - hb_last_ms >= 1000) { write_heartbeat(); hb_last_ms = now; }
    }

    g_eng.watchdog_run = false;
    if (g_eng.watchdog_thread.joinable()) g_eng.watchdog_thread.join();   // join before teardown nulls srv
    // Controlled teardown before `srv` (a main() local captured by the bus sink +
    // g_eng.srv_for_bp) leaves scope, and while the ImagePool/TriggerBus singletons
    // are still alive — covers exits that didn't go through cmd:shutdown (e.g.
    // g_eng.should_exit flipped elsewhere). Single source of truth; idempotent with
    // the shutdown handler. Runs BEFORE srv.stop() so the pool's workers are
    // joined (no emit) before the server goes away.
    controlled_shutdown_teardown_();
    srv.stop();
    std::fprintf(stderr, "[xinsp2] shutdown complete\n");
    return 0;
}
