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
#include <xi/xi_script_loader.hpp>
#include <xi/xi_ws_server.hpp>

#include <condition_variable>
#include <filesystem>
#include <thread>

#include <windows.h>

namespace xp = xi::proto;

static std::atomic<int64_t> g_run_id{0};

// Loaded user script state. When null, cmd:run returns an error.
static xi::script::LoadedScript g_script;

static std::mutex               g_script_mu;

// Persistent cross-frame state — survives DLL reloads.
static std::string g_persistent_state_json = "{}";
// Schema version of the DLL that wrote g_persistent_state_json. The
// next DLL's xi_script_state_schema_version() is compared against
// this on restore — mismatch (and both non-zero) drops the state
// rather than letting set_state default-fill into a different shape.
// 0 means "unversioned" — restore proceeds without the check.
static int         g_persistent_state_schema = 0;

// Cache of every successful `cmd:set_param` value the backend pushed
// into the live script. compile_and_load replays these into the new
// DLL via xi_script_set_param so user-tuned slider values aren't
// silently reset to file-scope defaults across a recompile. Keyed by
// param name → JSON-encoded scalar (number / bool / string token,
// same shape as set_param's `value` arg). Protected by g_script_mu.
static std::unordered_map<std::string, std::string> g_param_cache;

// Cache of every successful script-side `cmd:set_instance_def` def the
// backend pushed into the live script. Exact sibling of g_param_cache:
// script-declared xi::Instance objects live in the script DLL's OWN
// registry and their file-scope ctors re-seed the SOURCE default def on
// every reload, so without replaying the operator-tuned/taught/calibrated
// def the hot-recompile loop silently reverts each instance to its source
// default. Keyed by instance name → def JSON (same shape as set_instance_def's
// `def` arg). Only the SCRIPT-instance path is cached — backend plugin-manager
// instances persist via InstanceRegistry across a script recompile. Protected
// by g_script_mu like g_param_cache.
static std::unordered_map<std::string, std::string> g_instance_def_cache;

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

// Defined after g_plugin_mgr (declared further down); records a per-instance
// process() crash so a crash loop is visible via get_state.
static void note_instance_crash_(const char* name, const char* why);

// Part III G2.1 — stamp the process-global crash culprit (xi::crash::g_culprit)
// with the instance/plugin the host is about to enter, plus that plugin's
// folder + dll so the FE can quarantine it on a death. Defined after
// g_plugin_mgr. Cheap on the dispatch hot path: a per-thread cache means the
// manager lock is taken only when the active plugin on this thread changes.
static void stamp_culprit_(const char* instance, const std::string& plugin);

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
struct StagedEmit {
    std::string      target;   // destination sink instance name
    xi::TriggerEvent rec;      // images map + meta_doc; host owns one ref to each
};
static thread_local std::vector<StagedEmit> g_staged;

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
                xi::DocRegistry::instance().retain(m);   // register at rc=1
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
static int use_process_cb(const char* name,
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

static int use_exchange_cb(const char* name, const char* cmd,
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
static xi_image_handle use_grab_cb(const char* /*name*/, int /*timeout_ms*/) {
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
//      trigger every g_timer_interval_ms so a SOURCE-LESS script still ticks (the
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
static std::atomic<bool>       g_continuous{false};
// FPS the most recent cmd:start was launched with. compile_and_load
// captures this to re-arm continuous mode at the same rate after the
// reload completes — without it, mid-run hot-reload would silently
// halt the stream.
static std::atomic<int>        g_continuous_fps{10};
// Live timer-tick interval (ms). The continuous timer thread reads this every
// loop, so the synthetic-tick rate can be retuned WHILE RUNNING (cmd:set_timer_fps)
// — 0 = trigger-only (no ticks). Seeded from cmd:start's fps / project.json
// runtime.timer_fps. Default 100 (10fps) matches the historical default.
static std::atomic<int>        g_timer_interval_ms{100};
// Reserve stack headroom so the crash filter can dump after a script
// STACK_OVERFLOW; called at the top of each inspect-running thread. Forwards to
// the extracted forensics leaf (xi_crash_dump.hpp).
static void reserve_fault_stack() { xi::crash::reserve_fault_stack(); }
// Synthetic-tick timer thread (`g_timer_thread`): pushes an empty event at the
// configured fps so scripts without a trigger source still get periodic
// dispatch. The worker threads themselves are per-lane (see GroupLane).
static std::thread              g_timer_thread;
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
// dtor backstops any early-return — see that header. We pass &g_continuous as the
// "keep going" flag so a stop unblocks a waiter.
using xi::EmitGate;
using xi::EmitTurn;
// Serialise cmd:run dispatch threads so vars arrive in run_id
// order. Threads queue up here and the watchdog operates on whichever
// one is currently inside run_one_inspection — only one at a time.
static std::mutex              g_run_mu;

// Structural owner for the detached cmd:run / one-shot inspect threads — owns the
// bump-before-detach + bail-if-shutting-down + drain-on-teardown protocol that was
// the shutdown-window UAF class when hand-copied at every site. Defined + unit-
// tested in xi_inflight_runs.hpp.
static xi::InflightRuns g_inflight;

// Crash breadcrumb model + minidump machinery moved to xi_crash_dump.hpp
// (xi::crash::). These thin forwarders keep the dispatch hot-path call sites
// (crash_ctx()/crash_set()/crash_set_phase()) terse and unchanged.
static xi::crash::Context& crash_ctx() { return xi::crash::ctx(); }
inline void crash_set(char* dst, size_t n, const char* src) { xi::crash::set(dst, n, src); }
inline void crash_set_phase(const char* phase) { xi::crash::set_phase(phase); }

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
static std::atomic<int>        g_watchdog_ms{0};
static constexpr int           WD_SLOTS = 64;     // max concurrent in-flight inspects tracked
// Per-slot inspect deadline (steady_clock epoch-ms); 0 = free. Written by the
// dispatch/run thread that owns the slot, read by the watchdog thread.
static std::atomic<int64_t>    g_wd_deadlines[WD_SLOTS];
static std::atomic<int>        g_watchdog_trips{0};
static std::thread             g_watchdog_thread;
static std::atomic<bool>       g_watchdog_run{false};
static const int               WATCHDOG_EXIT_CODE = 0x5744;  // 'WD' — backend self-exit on a hard trip

// Claim a free watchdog slot for `deadline` (steady-clock epoch-ms). Returns the
// slot index, or -1 if all slots are busy (then this inspect runs unwatched —
// only possible with >64 concurrent inspects, far beyond any real pool).
static int wd_arm(int64_t deadline) {
    for (int i = 0; i < WD_SLOTS; ++i) {
        int64_t expect = 0;
        if (g_wd_deadlines[i].compare_exchange_strong(expect, deadline)) return i;
    }
    return -1;
}
static void wd_disarm(int slot) { if (slot >= 0) g_wd_deadlines[slot].store(0); }
// (The watchdog loop scans the slots inline now — it needs the per-slot deadline
// values to snapshot which inspects it targeted, so a fresh frame that overruns
// during the grace isn't mistaken for the originally-stuck one. See the monitor
// thread below.)

// Server pointer for emits that happen off the serving thread (status_cb, the
// dropped-frame markers). Atomic so a worker/plugin thread loads it once and the
// shutdown null-out can't tear a read. A pointer load is a plain mov on x86-64 —
// no hot-path cost.
static std::atomic<xi::ws::Server*> g_srv_for_bp{nullptr};   // set in main

// ---- Trigger access (script callbacks) ---------------------------------
// Set by the worker thread (or run_one_inspection) before invoking the
// script. The script reads via xi::current_trigger() through the three
// trigger_*_cb functions below. thread_local so multiple parallel
// dispatch threads can each have their own current trigger.
static thread_local const xi::TriggerEvent* g_current_trigger = nullptr;

// A1: owning thread id of the in-flight CurrentTriggerScope — NON-thread-local
// (unlike g_current_trigger above) so any thread can tell "is a trigger active
// somewhere?" apart from "is one active on MY thread?". GetCurrentThreadId() is
// never 0 for a live thread, so 0 unambiguously means "no trigger in flight".
//
// This disambiguates the two cases a trigger thunk's `!g_current_trigger` branch
// used to conflate (see Problem A in docs/internals/core_fix_plan.md):
//   * g_inspect_tid == 0  → genuinely no trigger (plain cmd:run, timer tick):
//     keep the historical empty / XI_IMAGE_NULL semantics.
//   * g_inspect_tid != 0  → a trigger IS active, but the caller is on a DIFFERENT
//     thread — an xi::async task or #pragma omp body that read the ambient
//     trigger off the inspect thread. That is the silent-bug class; fail loud.
static std::atomic<unsigned long> g_inspect_tid{0};   // GetCurrentThreadId(), 0 = none

// A1: invoked from a trigger thunk's "no current trigger" branch. If a trigger is
// actually in flight (on another thread), the caller used current_trigger() off
// the inspect thread — abort with a named message in debug, log-once in release.
// If no trigger is in flight at all, returns quietly so the thunk preserves its
// pre-existing empty / XI_IMAGE_NULL semantics (legitimate cmd:run / timer paths).
static void warn_trigger_off_thread_() {
    if (g_inspect_tid.load(std::memory_order_acquire) == 0) return;   // genuinely no trigger
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
static inline void release_trigger_event_(xi::TriggerEvent& ev) {
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
struct CurrentTriggerScope {
    xi::TriggerEvent& ev_;   // non-const: dtor reset()s the event's DocRef
    explicit CurrentTriggerScope(xi::TriggerEvent& ev) : ev_(ev) {
        g_current_trigger = &ev;
        // A1: publish the owning thread id so a trigger thunk fired on another
        // thread can tell "wrong thread" (loud bug) from "no trigger" (legit).
        g_inspect_tid.store(GetCurrentThreadId(), std::memory_order_release);
    }
    ~CurrentTriggerScope() {
        g_inspect_tid.store(0, std::memory_order_release);
        g_current_trigger = nullptr;
        release_trigger_event_(ev_);
    }
    CurrentTriggerScope(const CurrentTriggerScope&) = delete;
    CurrentTriggerScope& operator=(const CurrentTriggerScope&) = delete;
};

// F7: RAII for the enqueue path's "release the event UNLESS it was handed off to a
// lane queue" discipline. enqueue_to_lane_ has several early-return / drop exits that
// must release, and a couple of success exits that move `ev` into the queue and must
// NOT. The old hand-written `rel()` made FORGETTING = leak; this inverts it —
// release-by-default, call dismiss() only where ownership is transferred. (A move
// leaves `ev` empty, so even a missed dismiss() releases nothing — never a
// double-free.) Same shape as DispatchPoolGuard.
struct TriggerEventReleaser {
    xi::TriggerEvent* ev_;   // null ⇒ dismissed (handed off)
    explicit TriggerEventReleaser(xi::TriggerEvent& ev) : ev_(&ev) {}
    void dismiss() { ev_ = nullptr; }
    ~TriggerEventReleaser() { if (ev_) release_trigger_event_(*ev_); }
    TriggerEventReleaser(const TriggerEventReleaser&) = delete;
    TriggerEventReleaser& operator=(const TriggerEventReleaser&) = delete;
};

// ---- staged-sink drain / flush (paired with stage_sink_emit_ above) -------------
// Drain WITHOUT delivering — release every staged item's image/doc refs. The
// backstop for paths that staged but won't flush (no script, inspect crash, early
// return): StagedEmitGuard runs it on scope exit.
static void drain_staged_emits_() {
    for (auto& it : g_staged) release_trigger_event_(it.rec);
    g_staged.clear();
}
struct StagedEmitGuard { ~StagedEmitGuard() { drain_staged_emits_(); } };

// Deliver every staged sink call, in call order, to its target's process() via the
// SEH-guarded inline path. Called inside the EmitTurn gate (after wait_turn) so the
// deliveries are serialized in frame order. Stamps the run/arrival id ($seq) onto
// each record so a sink can correlate the packet to its frame. Fire-and-forget: the
// reply is dropped. On return g_staged is empty so StagedEmitGuard then no-ops.
static void flush_staged_emits_(int64_t run_id) {
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
                    xi::DocRegistry::instance().retain(copy);   // register at rc=1 (our ref)
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
            if (deliver) xi::DocRegistry::instance().retain(deliver);
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

struct CurrentTriggerInfoC {        // mirrors xi::CurrentTriggerInfo (xi_use.hpp)
    xi_trigger_id id;
    int64_t       timestamp_us;
    int32_t       is_active;
    int32_t       _pad;             // align dequeued_at_us to 8 bytes
    int64_t       dequeued_at_us;   // worker-stamped on dequeue from its lane
};

static void trigger_info_cb(CurrentTriggerInfoC* out) {
    if (!out) return;
    if (!g_current_trigger) { warn_trigger_off_thread_(); *out = {{0,0}, 0, 0, 0, 0}; return; }
    out->id             = g_current_trigger->id;
    out->timestamp_us   = g_current_trigger->timestamp_us;
    out->is_active      = 1;
    out->_pad           = 0;
    out->dequeued_at_us = g_current_trigger->dequeued_at_us;
}

static xi_image_handle trigger_image_cb(const char* source) {
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

static int32_t trigger_sources_cb(char* buf, int32_t buflen) {
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
static int32_t trigger_leader_cb(char* buf, int32_t buflen) {
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
static void* trigger_meta_cb() {
    if (!g_current_trigger) { warn_trigger_off_thread_(); return nullptr; }
    if (!g_current_trigger->meta_doc) return nullptr;
    xi::DocRegistry::instance().retain(g_current_trigger->meta_doc.get());
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
static uint32_t owner_get_cb() {
    return (uint32_t)xi::ImagePool::current_owner();
}
static void owner_set_cb(uint32_t id) {
    xi::ImagePool::current_owner_ref() = (xi::ImagePoolOwnerId)id;
}

// The single script-facing host_api (image_* + doc_* over the live singleton
// ImagePool, with the trigger/emit hook installed). Shared by set_use_callbacks
// (wired into the script's g_use_host_api_) AND the A4 explicit-trigger entry
// (put into the xi_trigger_view so the SDK can resolve the passed image/meta
// handles). One instance so both paths address the same pool/registry.
static const xi_host_api* script_host_api_() {
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
struct StatusEntry { std::string text; int64_t ts_ms = 0; uint64_t seq = 0; };
static std::mutex                         g_status_mu;
static std::map<std::string, StatusEntry> g_status;
static std::atomic<uint64_t>              g_status_seq{0};

static int64_t status_now_ms() { return xi::wall_ms(); }   // wall: status ts

// Update the latest status for `who`. Coalesces no-op repeats (same text) so a
// component setting the same string every frame doesn't spam events. Always
// mirrors into this thread's crash breadcrumb; pushes a best-effort event.
static void set_status_internal(const std::string& who, const char* text) {
    std::string t = text ? text : "";
    crash_set(crash_ctx().last_status, sizeof(crash_ctx().last_status), t.c_str());
    uint64_t seq;
    {
        std::lock_guard<std::mutex> lk(g_status_mu);
        auto it = g_status.find(who);
        if (it != g_status.end() && it->second.text == t) return;  // coalesce
        seq = ++g_status_seq;
        g_status[who] = StatusEntry{t, status_now_ms(), seq};
    }
    if (auto* srv = g_srv_for_bp.load(std::memory_order_acquire)) {
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
static void status_cb(const char* text) {
    set_status_internal("@script", text);
}

// ---- Per-run Result (run_result event) --------------------------------------
// One Result per trigger: a signed status code + message. See
// docs/roadmap/run-result.md. Framework system-fail enum lives in a reserved band
// (<= -990000) the user API (xi::result) refuses to set.
enum : int {
    XI_SYS_DROPPED    = -999001,  // overflow: event dropped before it could run
    XI_SYS_NO_VERDICT = -999005,  // ran but script set no RESULT (v1.1 opt-in; unused in v1)
};

// The current run's result, written by the script via xi::result(code,msg)
// through result_cb. thread_local so parallel lanes don't clobber each other
// (same as g_run_frame_path_). Reset at the top of each inspect.
struct RunResult { int code = 0; std::string msg; bool set = false; };
static thread_local RunResult g_run_result;

// Lowest user-usable result code; anything <= this is the framework system-fail
// band (mirrors xi::kResultSystemBand in xi_result.hpp).
static constexpr int kResultSystemBand = -990000;

// Installed into the script DLL (xi_script_set_result_callback) so xi::result()
// records the one per-run verdict. The host is the trust boundary: a user code in
// the reserved system band is NOT accepted as-is — it's recorded as NA (0) with a
// visible warning + the offending code preserved in the message, so the mistake
// surfaces instead of masquerading as a real verdict.
static void result_cb(int code, const char* msg) {
    if (code <= kResultSystemBand) {
        if (auto* srv = g_srv_for_bp.load(std::memory_order_acquire)) {
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

// Emit a `run_result` wire event. Fields ride directly in the event data (same
// envelope shape as run_finished). Used by the inspect path (run_id >= 0) and the
// drop path (run_id < 0 → omitted; code = XI_SYS_DROPPED). ms < 0 omits "ms".
static void emit_run_result(xi::ws::Server& srv, int code, const std::string& msg,
                            int64_t run_id, int64_t ms,
                            const std::string& source, const std::string& group) {
    std::string data = "{\"code\":" + std::to_string(code) + ",\"msg\":";
    xp::json_escape_into(data, msg);
    if (run_id >= 0) data += ",\"run_id\":" + std::to_string((long long)run_id);
    if (ms >= 0)     data += ",\"ms\":" + std::to_string((long long)ms);
    if (!source.empty()) { data += ",\"source\":"; xp::json_escape_into(data, source); }
    if (!group.empty())  { data += ",\"group\":";  xp::json_escape_into(data, group); }
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
static void run_one_inspection(xi::ws::Server& srv,
                               int frame_hint = 1,
                               int64_t run_id = 0,
                               const std::string& frame_path = "",
                               int64_t emit_seq = -1,
                               EmitGate* gate = nullptr);  // null = no ordering gate
                                                           // (one-shot/cmd:run pass emit_seq=-1)

// Path resolution for the script compiler. Backend derives its own dir at
// startup and uses that to locate the xi headers we ship alongside the exe.
static std::string g_include_dir;
static std::string g_work_dir;
static std::string g_plugins_dir;
// Accelerator install roots, probed once at startup. Empty string =
// not installed → user scripts fall back to portable C++ for that path.
static std::string g_opencv_dir;
static std::string g_turbojpeg_root;
static std::string g_ipp_root;
// vcvars64.bat override (empty = let the compiler auto-find via auto_find_vcvars).
static std::string g_tc_vcvars;
// Canonical folder of the currently-open project (the one the user edits — NOT
// any .xinsp_work scratch). Set on open_project; used to read/write the
// per-project "toolchain" override block in its project.json.
static std::string g_project_folder;
// The xi include dir derived from the exe location at startup. Kept separate from
// g_include_dir so a project override can point elsewhere yet we can always fall
// back to the shipped headers.
static std::string g_include_dir_default;

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
struct TcComponent {
    std::string key;       // stable id: "include" | "opencv" | "turbojpeg" | "ipp" | "vcvars"
    std::string label;     // human label
    std::string ov_key;    // project.json toolchain field name
    std::string env_var;   // env var that also sets it ("" = none)
    std::string sentinel;  // relative file proving the dir is real ("" = path is a file)
    std::string path;      // resolved path (may be empty)
    std::string source;    // "override" | "env" | "default" | "none"
    bool exists = false;   // sentinel (or the file itself, for vcvars) present
    bool optional = false; // optional accelerator → missing is info, not error
};

// Read one string field from the "toolchain" object of <folder>/project.json.
static std::string read_toolchain_override_(const std::string& folder, const char* field) {
    if (folder.empty()) return {};
    std::ifstream in((std::filesystem::path(folder) / "project.json").string());
    if (!in) return {};
    std::stringstream ss; ss << in.rdbuf();
    std::string out;
    std::string s = ss.str();
    if (yyjson_doc* doc = yyjson_read(s.c_str(), s.size(), 0)) {
        yyjson_val* root = yyjson_doc_get_root(doc);
        if (yyjson_val* tc = yyjson_obj_get(root, "toolchain"); tc && yyjson_is_obj(tc))
            if (yyjson_val* k = yyjson_obj_get(tc, field); k && yyjson_is_str(k) && yyjson_get_str(k))
                out = yyjson_get_str(k);
        yyjson_doc_free(doc);
    }
    return out;
}

static bool tc_sentinel_ok_(const TcComponent& c) {
    if (c.path.empty()) return false;
    std::error_code ec;
    if (c.sentinel.empty())  // vcvars: the path IS the file
        return std::filesystem::exists(c.path, ec);
    return std::filesystem::exists(std::filesystem::path(c.path) / c.sentinel, ec);
}

// Build the live component list for `folder` (the open project; "" = no project,
// startup defaults only). `path` comes straight from override/probe so it's always
// accurate; `source` is best-effort labelling for the UI.
static std::vector<TcComponent> resolve_toolchain_components_(const std::string& folder) {
    using namespace xi::script::detail;
    std::vector<TcComponent> v(5);
    v[0] = { "include",   "xi headers",        "include_dir",    "",               "xi/xi.hpp",                  "", "", false, false };
    v[1] = { "opencv",    "OpenCV",            "opencv_dir",     "OpenCV_DIR",     "include/opencv2/core.hpp",   "", "", false, false };
    v[2] = { "turbojpeg", "libjpeg-turbo",     "turbojpeg_root", "TURBOJPEG_ROOT", "include/turbojpeg.h",        "", "", false, true  };
    v[3] = { "ipp",       "Intel IPP",         "ipp_root",       "IPP_ROOT",       "include/ippi.h",             "", "", false, true  };
    v[4] = { "vcvars",    "MSVC (vcvars64)",   "vcvars",         "",               "",                           "", "", false, false };

    for (auto& c : v) {
        std::string ov = read_toolchain_override_(folder, c.ov_key.c_str());
        if (!ov.empty()) { c.path = ov; c.source = "override"; }
        else {
            // Built-in probe (already honours env then default candidates).
            std::string probed;
            if      (c.key == "include")   probed = g_include_dir_default;
            else if (c.key == "opencv")    probed = probe_opencv_dir();
            else if (c.key == "turbojpeg") probed = probe_turbojpeg_root();
            else if (c.key == "ipp")       probed = probe_ipp_root();
            else if (c.key == "vcvars")    probed = auto_find_vcvars();
            c.path = probed;
            const char* e = c.env_var.empty() ? nullptr : std::getenv(c.env_var.c_str());
            if (!c.path.empty() && e && *e)   c.source = "env";
            else if (!c.path.empty())         c.source = "default";
            else                              c.source = "none";
        }
        c.exists = tc_sentinel_ok_(c);
    }
    return v;
}

// Apply a project's toolchain resolution to the global compiler paths. Called on
// open_project and after set_toolchain_override so the next compile + the
// generated IntelliSense config both pick up the override immediately.
static void resolve_toolchain_(const std::string& folder) {
    auto comps = resolve_toolchain_components_(folder);
    for (auto& c : comps) {
        if      (c.key == "include")   { if (c.source == "override") g_include_dir = c.path; else g_include_dir = g_include_dir_default; }
        else if (c.key == "opencv")    g_opencv_dir     = c.path;
        else if (c.key == "turbojpeg") g_turbojpeg_root = c.path;
        else if (c.key == "ipp")       g_ipp_root       = c.path;
        else if (c.key == "vcvars")    g_tc_vcvars      = (c.source == "override") ? c.path : std::string();
    }
    std::fprintf(stderr, "[xinsp2] toolchain resolved: opencv=%s turbojpeg=%s ipp=%s vcvars=%s\n",
                 g_opencv_dir.empty() ? "none" : g_opencv_dir.c_str(),
                 g_turbojpeg_root.empty() ? "none" : g_turbojpeg_root.c_str(),
                 g_ipp_root.empty() ? "none" : g_ipp_root.c_str(),
                 g_tc_vcvars.empty() ? "auto" : g_tc_vcvars.c_str());
}

// Render the health report as JSON for the toolchain_health command / UI.
static std::string toolchain_health_json_(const std::string& folder) {
    auto comps = resolve_toolchain_components_(folder);
    bool all_ok = true;
    std::string out = "{\"components\":[";
    for (size_t i = 0; i < comps.size(); ++i) {
        auto& c = comps[i];
        // ok rules: an explicit override that doesn't resolve is always an error
        // (the user pointed us somewhere wrong); a required component must exist;
        // an optional one that's simply absent is fine.
        bool ok;
        if (c.source == "override") ok = c.exists;
        else if (!c.optional)       ok = c.exists;
        else                        ok = true;
        if (!ok) all_ok = false;

        std::string hint;
        if (c.source == "override" && !c.exists)
            hint = c.sentinel.empty() ? "overridden path does not exist"
                                      : ("expected " + c.sentinel + " under this folder");
        else if (!c.exists && !c.optional)
            hint = c.key == "vcvars" ? "vcvars64.bat not found — install VS Build Tools (Desktop C++ workload)"
                                     : ("not found — set " + (c.env_var.empty() ? std::string("an override") : c.env_var) + " or fix the path");
        else if (!c.exists && c.optional)
            hint = "optional accelerator, not installed";

        if (i) out += ",";
        out += "{\"key\":";       xp::json_escape_into(out, c.key);
        out += ",\"label\":";     xp::json_escape_into(out, c.label);
        out += ",\"path\":";      xp::json_escape_into(out, c.path);
        out += ",\"source\":";    xp::json_escape_into(out, c.source);
        out += ",\"env_var\":";   xp::json_escape_into(out, c.env_var);
        out += ",\"ov_key\":";    xp::json_escape_into(out, c.ov_key);
        out += ",\"exists\":";    out += c.exists ? "true" : "false";
        out += ",\"optional\":";  out += c.optional ? "true" : "false";
        out += ",\"ok\":";        out += ok ? "true" : "false";
        out += ",\"hint\":";      xp::json_escape_into(out, hint);
        out += "}";
    }
    out += "],\"all_ok\":";
    out += all_ok ? "true" : "false";
    out += ",\"project\":";
    xp::json_escape_into(out, folder);
    out += "}";
    return out;
}

// Merge one override into the canonical project.json "toolchain" block. Empty
// value clears that key (revert to env/probe). Returns false (with `err`) if the
// project.json can't be read/parsed/written.
static bool write_toolchain_override_(const std::string& folder, const std::string& field,
                                      const std::string& value, std::string& err) {
    namespace fs = std::filesystem;
    if (folder.empty()) { err = "no project open"; return false; }
    fs::path pj = fs::path(folder) / "project.json";
    std::ifstream in(pj.string());
    if (!in) { err = "cannot read project.json"; return false; }
    std::stringstream ss; ss << in.rdbuf();
    in.close();
    std::string src = ss.str();
    yyjson_doc* idoc = yyjson_read(src.c_str(), src.size(), 0);
    if (!idoc) { err = "project.json is not valid JSON"; return false; }
    // yyjson read DOM is immutable — copy to a mutable doc to edit in place.
    yyjson_mut_doc* d = yyjson_doc_mut_copy(idoc, NULL);
    yyjson_doc_free(idoc);
    if (!d) { err = "project.json is not valid JSON"; return false; }
    yyjson_mut_val* root = yyjson_mut_doc_get_root(d);
    yyjson_mut_val* tc = root ? yyjson_mut_obj_get(root, "toolchain") : nullptr;
    if (!tc || !yyjson_mut_is_obj(tc)) {
        yyjson_mut_obj_remove_str(root, "toolchain");  // drop any non-object
        tc = yyjson_mut_obj_add_obj(d, root, "toolchain");
    }
    if (value.empty()) yyjson_mut_obj_remove_str(tc, field.c_str());
    else {
        yyjson_mut_obj_remove_str(tc, field.c_str());
        yyjson_mut_obj_add_strcpy(d, tc, field.c_str(), value.c_str());
    }
    // Drop an emptied toolchain object so we don't leave "toolchain":{} noise.
    if (yyjson_mut_obj_size(tc) == 0) yyjson_mut_obj_remove_str(root, "toolchain");
    char* printed = yyjson_mut_write(d, YYJSON_WRITE_PRETTY, NULL);
    bool ok = false;
    if (printed) {
        // Route through atomic_write: a torn write here truncates the canonical
        // project.json (whole-project config loss). atomic_write leaves the prior
        // file intact on any failure and only renames the complete new content.
        if (xi::atomic_write(pj, std::string(printed) + "\n")) ok = true;
        else err = "cannot write project.json";
        free(printed);
    } else err = "failed to serialize project.json";
    yyjson_mut_doc_free(d);
    return ok;
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
static void read_script_deps_(const std::string& folder,
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
static DLL_DIRECTORY_COOKIE g_proj_dll_dir = nullptr;
static void set_project_dll_search_(const std::string& folder) {
    if (g_proj_dll_dir) { RemoveDllDirectory(g_proj_dll_dir); g_proj_dll_dir = nullptr; }
    if (folder.empty()) return;
    int wn = MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), -1, nullptr, 0);
    if (wn <= 0) return;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), -1, w.data(), wn);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    g_proj_dll_dir = AddDllDirectory(w.c_str());
}

// Plugin manager (global)
static xi::PluginManager g_plugin_mgr;

// (forward-declared above use_process_cb) record a per-instance process() crash.
static void note_instance_crash_(const char* name, const char* why) {
    if (name) g_plugin_mgr.note_instance_crash(name, why ? why : "process() crashed");
}

// (forward-declared above use_process_inline_) Part III G2.1 culprit stamp.
// The plugin name -> {folder, dll} resolution needs the manager lock, so a
// thread_local cache resolves it ONCE per (plugin, thread) and re-resolves only
// when the active plugin changes — the per-frame process() hot path then costs
// just a string compare + the four strncpy inside set_culprit().
static void stamp_culprit_(const char* instance, const std::string& plugin) {
    thread_local std::string t_plugin, t_folder, t_dll;
    if (plugin != t_plugin) {
        t_plugin = plugin;
        t_folder.clear();
        t_dll.clear();
        if (!plugin.empty())
            g_plugin_mgr.plugin_location(plugin, t_folder, t_dll);  // lock only on change
    }
    xi::crash::set_culprit(instance ? instance : "", plugin.c_str(),
                           t_folder.c_str(), t_dll.c_str());
}

static std::atomic<bool> g_should_exit{false};
// T2: set at the end of controlled_shutdown_teardown_ so the console-control
// handler can wait for a clean teardown before the OS force-terminates on a
// window close / logoff / shutdown (which give only a short grace window).
static std::atomic<bool> g_teardown_done{false};

// CLI/env arg parsing (get_exe_dir / parse_port / parse_host / parse_watchdog_ms
// / parse_auth_secret / parse_str_flag / has_flag / parse_autostart_fps /
// parse_extra_plugin_dirs) moved to xi/xi_cli_args.hpp (namespace xi::cli).

static double now_seconds() { return xi::wall_us() / 1e6; }   // wall: pong ts

static void send_rsp_ok(xi::ws::Server& srv, int64_t id, std::string data_json = "") {
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
struct RecentError {
    int64_t     ts_ms = 0;
    std::string source;     // "rsp" / "log" / "event"
    std::string message;
    int64_t     cmd_id  = 0;   // 0 if unknown
    int64_t     run_id  = 0;   // 0 if unknown
};
static std::mutex                     g_recent_errors_mu;
static std::deque<RecentError>        g_recent_errors;
static constexpr size_t               kRecentErrorsCap = 64;

static int64_t now_ms_() { return xi::wall_ms(); }   // wall: RecentError ts

static void push_recent_error(std::string source, std::string message,
                              int64_t cmd_id = 0, int64_t run_id = 0) {
    RecentError e{ now_ms_(), std::move(source), std::move(message), cmd_id, run_id };
    std::lock_guard<std::mutex> lk(g_recent_errors_mu);
    g_recent_errors.push_back(std::move(e));
    while (g_recent_errors.size() > kRecentErrorsCap) g_recent_errors.pop_front();
}

static void send_rsp_err(xi::ws::Server& srv, int64_t id, std::string err) {
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
static void emit_error_log(xi::ws::Server& srv, const std::string& msg,
                           int64_t run_id = 0) {
    xp::LogMsg lm; lm.level = "error"; lm.msg = msg;
    srv.send_text(lm.to_json());
    push_recent_error("log", msg, /*cmd_id=*/0, run_id);
}

static void send_hello(xi::ws::Server& srv) {
    xp::Event e;
    e.name = "hello";
    e.data_json = std::string(R"({"version":")") + XINSP2_VERSION
                + R"(","commit":")" + XINSP2_COMMIT
                + R"(","abi":1})";
    srv.send_text(e.to_json());
}


// (seh_exception and seh_translator defined above, before use_process_cb)

// Run one full inspection cycle: reset → inspect → emit.
// The inspect call is wrapped in SEH so a script crash (null deref,
// divide-by-zero, stack overflow) is caught without killing the backend.
static void run_one_inspection(xi::ws::Server& srv, int frame_hint,
                               int64_t run_id, const std::string& frame_path,
                               int64_t emit_seq, EmitGate* gate) {
    if (run_id == 0) run_id = ++g_run_id;

    // Claim the ordered-emit turn NOW (inert — wait_turn() happens at the emit so the
    // compute stays parallel). Its destructor releases the turn on EVERY exit below,
    // so an early-return (no script / inspect error / a future one) can't orphan the
    // sequence and deadlock the lane. No-op for emit_seq < 0.
    EmitTurn turn(gate, emit_seq, &g_continuous);

    // Drains any sink calls this inspect staged but didn't flush (no script, crash,
    // early return). The success path flushes (empties g_staged) so this no-ops.
    StagedEmitGuard staged_guard;

    xi::script::LoadedScript s;
    {
        std::lock_guard<std::mutex> lk(g_script_mu);
        s = g_script;
    }

    if (!s.ok()) {
        xp::LogMsg lm;
        lm.level = "warn";
        lm.msg = "no script loaded — compile a .cpp first";
        srv.send_text(lm.to_json());
        return;
    }

    // Plumb the optional per-run context (frame_path) into the script
    // DLL's globals before inspect runs. Cleared on the way out so a
    // subsequent run with no frame_path arg sees an empty string,
    // not the previous value.
    if (s.set_run_context) s.set_run_context(frame_path.c_str());

    // Per-run Result: reset to NA before the script runs, and snapshot the
    // source/group provenance from this thread's trigger (thread_local, valid
    // for the duration of the inspect). The script sets the result via
    // xi::result() → result_cb → g_run_result; we emit it below in the gate.
    g_run_result = RunResult{};
    std::string rr_source, rr_group;
    if (g_current_trigger) { rr_source = g_current_trigger->leader_source; rr_group = g_current_trigger->group; }

    // F-P1-1: bracket the inspect with run_started / run_finished /
    // run_error events so SDK callers can observe lifecycle outside the
    // synchronous rsp path. Documented in docs/reference/ws-protocol.md.
    auto emit_run_event = [&srv, run_id](const char* name,
                                          const std::string& extra_data = "") {
        xp::Event ev;
        ev.name = name;
        std::string data = "{\"run_id\":" + std::to_string(run_id);
        if (!extra_data.empty()) { data += ","; data += extra_data; }
        data += "}";
        ev.data_json = data;
        srv.send_text(ev.to_json());
    };
    emit_run_event("run_started");

    auto t0 = std::chrono::steady_clock::now();
    // Arm the watchdog: claim a per-inspect slot holding this inspect's
    // deadline. Works for any dispatch_threads (N slots), unlike the old
    // single-slot scheme that had to skip N>1. Cleared in the post-inspect
    // path below regardless of throw. No thread handle is kept — a hard trip
    // exits the process (FE respawns) rather than TerminateThread'ing a worker
    // (which would leak the per-instance lock + risk heap corruption).
    int wd_slot = -1;
    int wd_ms = g_watchdog_ms.load();
    if (wd_ms > 0) {
        // D-P1-10: deadline must use steady_clock (monotonic) — a system_clock
        // NTP/DST jump would skip every deadline or hang the watchdog forever.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wd_ms);
        wd_slot = wd_arm(std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline.time_since_epoch()).count());
    }
    auto disarm = [&]() { wd_disarm(wd_slot); wd_slot = -1; };
    // Stamp this dispatch thread's crash breadcrumb so a fault inside
    // the inspect (the most common crash site) names the run + phase
    // for THIS thread, not whatever the last thread to touch the
    // global wrote. frame_hint doubles as the per-thread frame marker.
    {
        auto& c = crash_ctx();
        c.last_run_id = run_id;   // int64 — keep the full id in crash reports
        c.last_frame  = frame_hint;
        crash_set(c.last_cmd, sizeof(c.last_cmd), "inspect");
    }
    // Run the inspect (in parallel under N>1). Capture success/error WITHOUT
    // emitting yet — emission happens below under the EmitTurn gate so ordered
    // mode (parallelism.result_order: "arrival") can serialize the wire stream
    // by frame-arrival order. Logs (diagnostic) stay immediate; the run_error
    // EVENT is deferred so it's ordered with run_finished and the turn always
    // advances (an error must not stall the ordered stream).
    bool inspect_ok = false;
    std::string run_error_what;   // set on failure; empty on success
    try {
        // Tag any image_create calls inside the script's inspect (and
        // any plugin process_fn it transitively calls that didn't set
        // its own guard) with the script's owner_id. Per-script
        // sweep-on-unload + per-instance sweep-on-destroy together
        // catch the leaked-handle case from both directions.
        xi::ImagePool::OwnerGuard sg(s.owner_id);
        crash_set_phase("reset");
        if (s.reset) s.reset();
        crash_set_phase("inspect");
        // Draw this inspect's watchdog cancel ticket (epoch) on the dispatch
        // thread BEFORE running it. The watchdog's cooperative cancel targets
        // only inspects whose ticket predates a trip, so a fresh frame started
        // during the 1000ms grace (higher ticket) is not poisoned by an earlier
        // slow frame's trip. Older scripts lack this thunk → legacy behaviour.
        if (s.inspect_begin) s.inspect_begin();
        // A4: prefer the EXPLICIT-trigger entry when the script exports it. The
        // host builds a self-contained xi_trigger_view from the current trigger
        // (read once, HERE, on the inspect thread) and passes it in — so the
        // script never reaches for the ambient thread_local. Image handles + meta
        // doc are BORROWED (the dispatch's CurrentTriggerScope holds the refs);
        // the SDK Trigger addref's/retains its own before this call returns.
        // g_current_trigger == nullptr (plain cmd:run / timer tick) ⇒ an inactive
        // view, mirroring current_trigger().is_active() == false on the old path.
        if (s.inspect_tv) {
            xi_trigger_view view{};
            std::vector<xi_trigger_view_image> view_imgs;
            std::string leader;
            if (g_current_trigger) {
                view.is_active      = 1;
                view.id             = g_current_trigger->id;
                view.timestamp_us   = g_current_trigger->timestamp_us;
                view.dequeued_at_us = g_current_trigger->dequeued_at_us;
                view_imgs.reserve(g_current_trigger->images.size());
                for (auto& kv : g_current_trigger->images)
                    view_imgs.push_back({ kv.first.c_str(), kv.second });
                view.images       = view_imgs.data();
                view.image_count  = (int32_t)view_imgs.size();
                leader            = g_current_trigger->leader_source;
                view.leader_source = leader.c_str();
                view.meta_doc     = (void*)g_current_trigger->meta_doc.get();
            }
            view.host = script_host_api_();
            s.inspect_tv(&view, frame_hint);
        } else {
            s.inspect(frame_hint);
        }
        crash_set_phase("done");
        disarm();
        inspect_ok = true;
    } catch (const seh_exception& e) {
        disarm();
        auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0).count();
        char msg[256];
        std::snprintf(msg, sizeof(msg), "script crashed after %lldms: 0x%08X (%s)",
                     (long long)dt_ms, e.code, e.what());
        std::fprintf(stderr, "[xinsp2] %s\n", msg);
        emit_error_log(srv, msg, run_id);
        run_error_what = "\"what\":";
        xp::json_escape_into(run_error_what, std::string(msg));
    } catch (const std::exception& e) {
        disarm();
        std::fprintf(stderr, "[xinsp2] inspect threw: %s\n", e.what());
        emit_error_log(srv, std::string("script exception: ") + e.what(), run_id);
        run_error_what = "\"what\":";
        xp::json_escape_into(run_error_what, std::string("script exception: ") + e.what());
    } catch (...) {
        disarm();
        // Align with the named catches above: also leave a stderr breadcrumb +
        // push to the recent-errors ring, else an unknown (non-std) throw vanished
        // from both be_log and cmd:recent_errors.
        std::fprintf(stderr, "[xinsp2] inspect threw a non-std exception (run_id=%lld)\n", (long long)run_id);
        emit_error_log(srv, "script threw a non-std exception", run_id);
        run_error_what = "\"what\":\"unknown_exception\"";
    }

    auto dt_us = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - t0).count();
    auto dt_ms = dt_us / 1000;   // wire/event latency stays integer-ms (unchanged)
    // OQ-7a observability: record this frame's latency + ok/error into the process
    // metrics registry (lock-free). Sub-ms precision from dt_us so fast frames don't
    // all collapse into the 0-bucket. Exported via cmd:metrics (see dispatch below).
    xi::MetricsRegistry::instance().record_frame((double)dt_us / 1000.0, inspect_ok);
    {
        // Ordered mode: block until it's this frame's turn to emit, then advance the
        // cursor (turn.complete()) right after so the next worker can emit promptly.
        // The turn was claimed at the top of this function; its dtor is the backstop
        // if anything below throws. No-op for emit_seq < 0 (completion mode).
        bool my_turn = turn.wait_turn();
        if (inspect_ok) {
            // Deliver this frame's staged sink calls (comm/expose/…) IN FRAME ORDER —
            // inside the gate, before the wire result, so a sink's side effect is
            // serialized with the run's output. A failed inspect skips this; the
            // guard then drops the partial sends (don't push a crashed frame to PLC).
            // On a STOP wake (my_turn false: the lane stopped before this seq's turn,
            // so every parked seq woke at once) also skip — flushing here would deliver
            // out of frame order and concurrently to the same sink. The staged guard
            // drops them, matching the crash path ("don't push this frame to the PLC").
            if (my_turn) flush_staged_emits_(run_id);
            // One Result per run: whatever the script set (default 0 = NA if it
            // called no xi::result), emitted before run_finished so consumers
            // can pair them. Ordered with the rest of the stream by the gate.
            emit_run_result(srv, g_run_result.code, g_run_result.msg,
                            run_id, dt_ms, rr_source, rr_group);
            emit_run_event("run_finished",
                           "\"ms\":" + std::to_string((long long)dt_ms));
            // Clear so the next run, if it doesn't carry a frame_path arg,
            // sees an empty path instead of the stale previous one.
            if (s.set_run_context) s.set_run_context("");
        } else {
            // Inspect failed (crash/throw) — still emit one Result so the stream
            // has no gap. v1: NA (0). v1.1 upgrades this to XI_SYS_CRASHED/TIMEOUT.
            emit_run_result(srv, 0, "inspect error", run_id, dt_ms, rr_source, rr_group);
            emit_run_event("run_error", run_error_what);
        }
        turn.complete();   // advance the gate now (dtor would otherwise do it at fn exit)
    }
}

// (trigger_worker removed — continuous mode uses a simple timer thread)

// ---- Dispatch groups: per-group worker lanes (gated on parallelism.groups) ----
// Each group owns its own queue + max_parallel worker threads at its OS
// thread_priority, draining only its own queue. Only active when the project
// declares groups; with no groups a single synthesized default lane runs everything (the old separate single pool is gone — see lane_for_()).
// Result ordering is per-lane completion order in v1 (per-group arrival + the
// `group` wire tag are follow-ups). See docs/internals/dispatch.md.
// P1-8: lifetime-cumulative dispatch counters. The per-lane dropped/high_watermark
// reset on every cmd:start (lanes are recreated by spawn_group_pool_), which erases
// the "how much did we drop last run" history — a restart looks clean. These
// process-globals persist for the whole backend uptime (like ImagePool's
// total_created_), so a monitor can see total drops / peak depth across run
// boundaries. dispatch_stats reports them as *_lifetime alongside the per-run ones.
static std::atomic<uint64_t> g_dropped_lifetime{0};
static std::atomic<uint64_t> g_high_watermark_lifetime{0};   // max single-lane depth ever seen

struct GroupLane {
    xi::ProjectInfo::DispatchGroup cfg;
    std::deque<xi::TriggerEvent>   q;
    std::mutex                     mu;
    std::condition_variable        cv;
    std::vector<std::thread>       workers;
    std::atomic<uint64_t>          running{0};
    std::atomic<uint64_t>          dropped{0};
    std::atomic<uint64_t>          high_watermark{0};
    // Per-group result ordering (result_order: "arrival"). `ordered` is set at
    // spawn (arrival && max_parallel>1). Each dequeue claims a gapless seq from
    // seq_next under mu (so it follows arrival order); `gate` replays emission in
    // that order — independent of other groups' gates.
    bool                           ordered{false};
    std::atomic<int64_t>           seq_next{0};
    EmitGate                       gate;
    // Rate limit (min_interval_ms): the next steady-clock-us a dispatch may START.
    // Workers CAS-claim a slot ≥ this and sleep to it, so dispatch starts are ≥
    // min_interval apart across the lane (surplus events coalesce via drop_oldest).
    std::atomic<int64_t>           next_allowed_us{0};
};
// Lanes are shared_ptr + guarded by g_lanes_mu so a producer (an emit thread /
// the timer) that grabbed a lane can't have it destroyed under it by a concurrent
// stop_group_pool_ — the shared_ptr keeps the GroupLane (its mutex/cv) alive until
// the producer is done. Fixes the lane-lifetime UAF found in v1 hardening.
static std::vector<std::shared_ptr<GroupLane>> g_lanes;
static std::mutex                              g_lanes_mu;
// F4: default_group as captured WHEN the current lane set was spawned (guarded by
// g_lanes_mu, set in spawn_group_pool_). lane_for_ routes against THIS, not a live
// model read — so routing can never reference a group name that isn't in g_lanes.
// Today groups are load-only (open_project quiesces + respawns), so the live value
// can't diverge from the lanes; reading the snapshot makes that self-consistent by
// construction instead of by that external invariant — the safe shape for when a
// runtime "reconfigure groups" command is eventually added.
static std::string                             g_default_group_snapshot;   // guarded by g_lanes_mu

static void set_os_thread_priority_(const std::string& p) {
#ifdef _WIN32
    int pr = THREAD_PRIORITY_NORMAL;
    if      (p == "high") pr = THREAD_PRIORITY_ABOVE_NORMAL;
    else if (p == "low")  pr = THREAD_PRIORITY_BELOW_NORMAL;
    SetThreadPriority(GetCurrentThread(), pr);
#else
    (void)p;   // TODO(linux): pthread_setschedprio / nice
#endif
}

// Pin the current thread to a set of cores (a MASK — the thread may run on ANY of
// them, not just one). Empty `cores` = leave unbound. Bogus core ids are dropped
// (intersected with the process's allowed mask) so a bad config can't wipe the
// affinity to nothing.
static void set_os_thread_affinity_(const std::vector<int>& cores) {
    if (cores.empty()) return;
#ifdef _WIN32
    DWORD_PTR want = 0;
    for (int c : cores) if (c >= 0 && c < 64) want |= (DWORD_PTR(1) << c);  // 64-bit mask (≤64 cores; >64 = processor groups, TODO)
    if (!want) return;
    DWORD_PTR procMask = 0, sysMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask)) want &= procMask;
    if (want) SetThreadAffinityMask(GetCurrentThread(), want);
#else
    (void)cores;   // TODO(linux): cpu_set_t + pthread_setaffinity_np / sched_setaffinity
#endif
}

// Set the backend PROCESS priority class (Win). Returns true if applied. Used at
// startup (--priority) and live (cmd:set_process_priority). "" = leave unchanged.
static bool apply_process_priority_(const std::string& cls) {
    if (cls.empty()) return false;
#ifdef _WIN32
    DWORD c = 0;
    if      (cls == "high")     c = HIGH_PRIORITY_CLASS;
    else if (cls == "above")    c = ABOVE_NORMAL_PRIORITY_CLASS;
    else if (cls == "normal")   c = NORMAL_PRIORITY_CLASS;
    else if (cls == "below")    c = BELOW_NORMAL_PRIORITY_CLASS;
    else if (cls == "realtime") c = REALTIME_PRIORITY_CLASS;
    else return false;
    SetPriorityClass(GetCurrentProcess(), c);
    std::fprintf(stderr, "[xinsp2] process priority = %s\n", cls.c_str());
    return true;
#else
    (void)cls; return false;   // TODO(linux): setpriority/sched_setscheduler
#endif
}

// Warn (once per start) if the total dispatch worker count exceeds the core count.
// Oversubscription causes context-switch thrash that usually slows inspects — a
// dedicated inspection PC should keep Σ max_parallel ≤ cores (minus a couple for
// the FE supervisor / OS).
static void warn_oversubscribe_(int total_workers) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw > 0 && total_workers > (int)hw)
        std::fprintf(stderr,
            "[xinsp2] WARNING: %d dispatch worker threads on %u cores (oversubscribed) — "
            "context-switch thrash may slow inspects; lower max_parallel or add cores\n",
            total_workers, hw);
}

// Resolve a group name to its lane (holding g_lanes_mu). Unknown/typo'd group →
// the default_group lane, then the first lane — never silently the front (#5).
// Returns a shared_ptr so the caller keeps the lane alive past a concurrent stop.
static std::shared_ptr<GroupLane> lane_for_(const std::string& group) {
    std::lock_guard<std::mutex> lk(g_lanes_mu);
    if (g_lanes.empty()) return nullptr;
    for (auto& l : g_lanes) if (l->cfg.name == group) return l;
    const std::string& dg = g_default_group_snapshot;   // F4: spawn-time snapshot, not a live read
    if (!dg.empty()) for (auto& l : g_lanes) if (l->cfg.name == dg) return l;
    return g_lanes.front();
}

// Frame drops are reported to a connected client via emit_run_result, but an
// unattended factory PC with no UI would degrade SILENTLY (be_log stays clean
// while the system throws away work). Leave a throttled breadcrumb in stderr too.
// Powers-of-2 throttle: 1,2,4,8,… — visible from the first drop, never spammy.
static void warn_frame_drop_(uint64_t dropped, const std::string& group, const char* policy) {
    if (dropped == 0) return;
    if (dropped == 1 || (dropped & (dropped - 1)) == 0)
        std::fprintf(stderr,
            "[xinsp2] WARN dropping frames (group='%s', policy=%s, dropped=%llu) — source out-running processing\n",
            group.empty() ? "(default)" : group.c_str(), policy, (unsigned long long)dropped);
}

// Shared drop accounting for the two back-pressure paths that discard a frame:
// the continuous lane's drop_newest branch (enqueue_to_lane_) and the one-shot
// max_inflight cap (dispatch_one_shot_). Both re-derived the identical tail:
// release the dropped event's image + doc refs, warn (throttled), then emit the
// out-of-band XI_SYS_DROPPED marker. Consolidated here so the marker shape can't
// drift between the two. The bits that genuinely DIFFER stay at the call sites and
// ride in as params: `warn_count` is the counter the throttle reads (the per-LANE
// dropped total for a lane — read under the lane lock — vs the process-lifetime
// total for the capless one-shot path), `aid` is the arrival slot (claimed under
// the lane lock for drop_newest, so it stays at the call site to preserve ordering),
// and `policy`/`reason` carry each site's exact marker payload byte-for-byte. The
// counter bumps (g_dropped_lifetime, and lane->dropped for the lane path) also
// stay at the call sites — the two paths bump different counter SETS and the lane
// counter must be touched under the lane lock.
static void account_dropped_frame_(xi::TriggerEvent& ev, uint64_t warn_count,
                                   int64_t aid, const char* policy, const char* reason) {
    release_trigger_event_(ev);            // the dropped event's image + doc refs
    warn_frame_drop_(warn_count, ev.group, policy);
    if (auto* srv = g_srv_for_bp.load(std::memory_order_acquire))
        emit_run_result(*srv, XI_SYS_DROPPED, reason, aid, -1, ev.leader_source, ev.group);
}

// Per-lane enqueue with that lane's queue_depth/overflow policy.
static bool enqueue_to_lane_(xi::TriggerEvent ev) {
    // F7: releases ev on EVERY exit unless dismiss()'d (i.e. handed to a lane queue).
    TriggerEventReleaser guard(ev);
    if (!g_continuous.load()) return false;
    std::shared_ptr<GroupLane> lane = lane_for_(ev.group);
    if (!lane) return false;
    int depth = lane->cfg.queue_depth < 1 ? 1 : lane->cfg.queue_depth;
    const std::string& ov = lane->cfg.overflow;
    std::unique_lock<std::mutex> lk(lane->mu);
    // Re-check after taking the lane lock: a concurrent stop may have flipped
    // g_continuous + drained; don't push a now-orphaned event (would leak).
    if (!g_continuous.load()) return false;
    if ((int)lane->q.size() < depth) {
        ev.arrival_id = ++g_run_id;   // arrival/run id in push (== FIFO) order
        lane->q.push_back(std::move(ev)); guard.dismiss();   // ownership → queue
        uint64_t ns = lane->q.size(), prev = lane->high_watermark.load(std::memory_order_relaxed);
        while (ns > prev && !lane->high_watermark.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {}
        // P1-8: also raise the process-lifetime peak (survives cmd:start).
        uint64_t gprev = g_high_watermark_lifetime.load(std::memory_order_relaxed);
        while (ns > gprev && !g_high_watermark_lifetime.compare_exchange_weak(gprev, ns, std::memory_order_relaxed)) {}
        lane->cv.notify_one(); return true;
    }
    if (ov == "drop_newest") {
        ++lane->dropped; ++g_dropped_lifetime;   // P1-8: lifetime total survives cmd:start
        int64_t aid = ++g_run_id;   // arrival slot of the dropped (new) frame
        uint64_t wc = lane->dropped.load();   // throttle reads the per-lane total (under lock)
        lk.unlock();
        guard.dismiss();   // account_dropped_frame_ releases the dropped (new) ev
        // Out-of-band NA marker (not gated — gating would stall the source); the
        // run_id lets a consumer order it against this lane's run results.
        account_dropped_frame_(ev, wc, aid, "drop_newest", "dropped: queue full (drop_newest)");
        return false;
    }
    auto& front = lane->q.front();   // drop_oldest (the default + fallback)
    int64_t dropped_aid = front.arrival_id;   // the dropped (oldest) frame's slot
    std::string ds = front.leader_source, dg = front.group;   // the dropped (oldest) event
    release_trigger_event_(front);   // the evicted front (a different event, in-queue)
    lane->q.pop_front(); ev.arrival_id = ++g_run_id; lane->q.push_back(std::move(ev)); guard.dismiss();   // ev → queue
    lane->cv.notify_one(); ++lane->dropped; ++g_dropped_lifetime;   // P1-8
    warn_frame_drop_(lane->dropped.load(), dg, "drop_oldest");
    lk.unlock();
    if (auto* srv = g_srv_for_bp.load(std::memory_order_acquire))
        emit_run_result(*srv, XI_SYS_DROPPED, "dropped: queue full (drop_oldest)", dropped_aid, -1, ds, dg);
    return true;
}

static void spawn_group_pool_(xi::ws::Server* srv_ptr, int interval_ms) {
    {
        std::lock_guard<std::mutex> lk(g_lanes_mu);
        // F8: callers must stop_dispatch_pool_ before (re)spawning — a non-empty
        // g_lanes here means a prior pool's workers are still running and we'd
        // silently double-spawn (two worker sets draining one source). All sites
        // pair stop+spawn today; assert the invariant so a future site can't
        // regress it silently. Release builds also leave a stderr breadcrumb since
        // assert() is compiled out there.
        assert(g_lanes.empty() && "spawn_group_pool_ called without a preceding stop_dispatch_pool_");
        if (!g_lanes.empty())
            std::fprintf(stderr, "[xinsp2] BUG: spawn_group_pool_ with %zu live lane(s) — "
                         "missing stop_dispatch_pool_; clearing (workers may double-run)\n",
                         g_lanes.size());
        g_lanes.clear();
        // F4: capture default_group with this lane set so lane_for_ routes against
        // a name that exists in g_lanes (the synthesized default lane below is "").
        g_default_group_snapshot = g_plugin_mgr.project().default_group;
        for (auto& gc : g_plugin_mgr.project().groups) {
            auto lane = std::make_shared<GroupLane>(); lane->cfg = gc; g_lanes.push_back(std::move(lane));
        }
        if (g_lanes.empty()) {
            // No explicit groups: synthesize ONE default lane from the project's
            // parallelism settings, so every project runs on the unified lane
            // path (the legacy single pool is gone). name "" matches an untagged
            // event's empty group via lane_for_(); the timer tick also targets
            // default_group (== "" here) -> this lane.
            const auto& p = g_plugin_mgr.project();
            xi::ProjectInfo::DispatchGroup def;
            def.name         = "";
            def.max_parallel = p.dispatch_threads < 1 ? 1 : p.dispatch_threads;
            def.queue_depth  = p.queue_depth;
            def.overflow     = p.overflow;
            def.result_order = p.result_order;
            auto lane = std::make_shared<GroupLane>(); lane->cfg = def;
            g_lanes.push_back(std::move(lane));
        }
    }
    std::fprintf(stderr, "[xinsp2] continuous mode (grouped): %zu group(s), %dms timer\n",
                 g_lanes.size(), interval_ms);
    { int total = 0; for (auto& lp : g_lanes) total += (lp->cfg.max_parallel < 1 ? 1 : lp->cfg.max_parallel);
      warn_oversubscribe_(total); }
    for (auto& lp : g_lanes) {
        std::shared_ptr<GroupLane> lane = lp;   // workers hold a ref → lane outlives them
        int n = lane->cfg.max_parallel < 1 ? 1 : lane->cfg.max_parallel;
        // Arrival-ordered emission only when asked AND there's real concurrency
        // (n==1 is already in order). Reset the per-lane cursors per (re)start.
        lane->ordered = (lane->cfg.result_order == "arrival") && n > 1;
        lane->seq_next.store(0);
        lane->next_allowed_us.store(0);
        { std::lock_guard<std::mutex> lk(lane->gate.mu); lane->gate.next = 0; }
        for (int i = 0; i < n; ++i) {
            lane->workers.emplace_back([srv_ptr, lane, wi = i] {
                reserve_fault_stack();
                xi::install_seh_translator();
                set_os_thread_priority_(lane->cfg.thread_priority);
                // CPU affinity (empty = unbound). One mask → all workers share it;
                // N masks → worker wi uses set[wi % N]. Each mask may be multi-core.
                const auto& aff = lane->cfg.cpu_affinity;
                if (!aff.empty())
                    set_os_thread_affinity_(aff.size() == 1 ? aff[0] : aff[(size_t)wi % aff.size()]);
                while (g_continuous.load()) {
                    xi::TriggerEvent ev; bool have = false; int64_t rid = 0; int64_t eseq = -1;
                    {
                        std::unique_lock<std::mutex> lk(lane->mu);
                        lane->cv.wait(lk, [lane] { return !lane->q.empty() || !g_continuous.load(); });
                        if (!g_continuous.load()) break;
                        if (!lane->q.empty()) {
                            ev = std::move(lane->q.front()); lane->q.pop_front(); have = true;
                            // run_id was claimed at ENQUEUE (push == FIFO order) so kept
                            // and dropped frames share one arrival sequence; read it back
                            // (fallback if unset). The emit seq (gate ordering) is still
                            // claimed here (ordered mode) — drops leave no gate gap.
                            rid = ev.arrival_id ? ev.arrival_id : ++g_run_id;
                            if (lane->ordered) eseq = lane->seq_next.fetch_add(1);
                        }
                    }
                    // Only lane WORKERS wait on this cv now (the overflow:block
                    // producer-park was removed), so notify_one is correct + cheaper:
                    // one freed slot wakes one worker.
                    lane->cv.notify_one();
                    if (!have) continue;
                    // Rate limit: CAS-claim a dispatch slot ≥ min_interval after the
                    // lane's previous one, then sleep to it. Surplus events meanwhile
                    // pile in the queue and coalesce via drop_oldest (latest wins).
                    if (lane->cfg.min_interval_ms > 0) {
                        // steady clock: a wall-clock (NTP/DST) jump must not stall
                        // the lane for the duration of the jump. next_allowed_us
                        // holds steady-us (reset to 0 at lane start).
                        int64_t iv = (int64_t)lane->cfg.min_interval_ms * 1000;
                        int64_t now = xi::steady_now_us(), prev = lane->next_allowed_us.load(std::memory_order_relaxed), slot;
                        do { slot = prev > now ? prev : now; }
                        while (!lane->next_allowed_us.compare_exchange_weak(prev, slot + iv, std::memory_order_acq_rel));
                        for (int64_t w = slot - xi::steady_now_us(); w > 0 && g_continuous.load(); w = slot - xi::steady_now_us())
                            std::this_thread::sleep_for(std::chrono::microseconds(w > 20000 ? 20000 : w));
                    }
                    ev.dequeued_at_us = xi::now_us();
                    lane->running.fetch_add(1);
                    int frame_seq = (int)rid;
                    if (!ev.images.empty() || ev.id.hi || ev.id.lo) {
                        CurrentTriggerScope trig(ev);   // clears g_current_trigger + releases ev on scope exit
                        run_one_inspection(*srv_ptr, frame_seq, rid, "", eseq, &lane->gate);
                    } else {
                        run_one_inspection(*srv_ptr, frame_seq, rid, "", eseq, &lane->gate);
                    }
                    lane->running.fetch_sub(1);
                }
            });
        }
    }
    // Timer ticks feed the default group's lane. Always spawned; reads the LIVE
    // g_timer_interval_ms each loop so the rate is retunable mid-run and 0 =
    // trigger-only (the default group isn't loaded with synthetic ticks).
    (void)interval_ms;
    g_timer_thread = std::thread([] {
        const std::string dg = g_plugin_mgr.project().default_group;
        while (g_continuous.load()) {
            int iv = g_timer_interval_ms.load();
            if (iv <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(iv));
            if (!g_continuous.load()) break;
            if (g_timer_interval_ms.load() <= 0) continue;
            xi::TriggerEvent ev; ev.group = dg;
            (void)enqueue_to_lane_(std::move(ev));
        }
    });
}

static void stop_group_pool_() {
    // Snapshot the lanes (under the lock) so producers can keep routing into the
    // shared_ptrs while we tear down; new enqueues already bail on !g_continuous.
    std::vector<std::shared_ptr<GroupLane>> lanes;
    { std::lock_guard<std::mutex> lk(g_lanes_mu); lanes = g_lanes; }
    for (auto& lp : lanes) { std::lock_guard<std::mutex> lk(lp->mu); lp->cv.notify_all(); }
    for (auto& lp : lanes) for (auto& t : lp->workers) if (t.joinable()) t.join();
    // Workers are gone + g_continuous is false → drain leftover queued events and
    // release their image handles before the lanes are dropped (release-before-
    // FreeLibrary; #3 leak fix).
    for (auto& lp : lanes) {
        std::lock_guard<std::mutex> lk(lp->mu);
        for (auto& ev : lp->q) release_trigger_event_(ev);
        lp->q.clear();
    }
    { std::lock_guard<std::mutex> lk(g_lanes_mu); g_lanes.clear(); }
}

// Stop the pool + timer. Safe to call if nothing was spawned.
static void stop_dispatch_pool_() {
    g_continuous = false;
    // Wake the lane workers (so they observe g_continuous=false and exit) BEFORE
    // joining, or the join deadlocks. Also wake anyone parked in a per-lane EmitTurn
    // (ordered mode).
    {
        std::vector<std::shared_ptr<GroupLane>> lanes;
        { std::lock_guard<std::mutex> lk(g_lanes_mu); lanes = g_lanes; }
        for (auto& lp : lanes) {
            { std::lock_guard<std::mutex> lk(lp->mu); lp->cv.notify_all(); }
            { std::lock_guard<std::mutex> lk(lp->gate.mu); lp->gate.cv.notify_all(); }
        }
    }
    if (g_timer_thread.joinable()) g_timer_thread.join();
    stop_group_pool_();
}

// Trigger-driven dispatch WITHOUT continuous mode: a source emitting a trigger
// (e.g. a webui "issue"/"replay" click) runs exactly ONE inspect on it. The emit
// usually arrives on the WS thread (inside a plugin's exchange), so we run the
// inspect on a detached thread, not inline. Serialized by g_run_mu; the
// thread_local g_current_trigger makes this thread's inspect see this event.
static void dispatch_one_shot_(xi::ws::Server* srv, xi::TriggerEvent ev) {
    auto evp = std::make_shared<xi::TriggerEvent>(std::move(ev));
    // The lambda runs on a source plugin's emit thread, which outlives the
    // main-local srv — g_inflight owns the bump/bail/drain so teardown waits it
    // out. On a bail (tearing down or thread-spawn failure) OR a cap drop we
    // release the event's image/meta handles ourselves.
    bool dropped_over_cap = false;
    bool launched = g_inflight.launch([srv, evp]() {
        reserve_fault_stack();
        xi::install_seh_translator();
        std::lock_guard<std::mutex> lk(g_run_mu);
        CurrentTriggerScope trig(*evp);   // clears g_current_trigger + releases evp on scope exit
        run_one_inspection(*srv, /*frame_hint=*/0, /*run_id=*/0, "", /*emit_seq=*/-1);
    }, &dropped_over_cap);
    if (!launched) {
        // B1: on ANY non-launch we must release the dropped event's image/meta refs
        // ourselves (the CurrentTriggerScope that normally does it never ran) — the
        // same release-on-drop discipline the continuous lane path uses, so a
        // dropped one-shot leaks no ImagePool/DocRegistry handles.
        if (dropped_over_cap) {
            // At the in-flight cap this is an OVERFLOW drop-newest: mirror the
            // continuous GroupLane drop_newest path via the shared helper — bump the
            // lifetime dropped counter and emit an out-of-band XI_SYS_DROPPED marker
            // (release happens inside the helper) so the drop is observable/counted.
            int64_t aid = ++g_run_id;              // arrival slot of the dropped (new) frame
            ++g_dropped_lifetime;                  // P1-8: lifetime total survives cmd:start
            account_dropped_frame_(*evp, g_dropped_lifetime.load(), aid, "max_inflight",
                                   "dropped: max in-flight one-shots reached");
        } else {
            // A bare shutdown/pause bail is not a drop (no marker) — just release.
            release_trigger_event_(*evp);
        }
    }
}

// SINGLE SOURCE OF TRUTH for process-exit teardown. Called from BOTH the
// cmd:shutdown handler and the main() epilogue (which covers exits that didn't go
// through cmd:shutdown). Previously this sequence was hand-copied in both places
// and had drifted — the epilogue copy was missing reset() and never stopped the
// dispatch pool — which is exactly the class of teardown bug the audit rounds
// kept hitting. Run it once, in this fixed order:
//   1. stop every emit SOURCE first (dispatch pool, replay, in-flight detached
//      run) so no inspect emits after this point,
//   2. drop the bus sink/observer + release bus-held image handles,
//   3. unload the script under its lock (its module deleter runs while the
//      ImagePool is still alive),
//   4. drop the srv pointer LAST (after every emitter is quiesced).
// Idempotent: safe to call twice (the shutdown handler runs it, then the epilogue
// runs it again as the loop unwinds).
static void controlled_shutdown_teardown_() {
    // Refuse NEW detached runs first, then drop the bus sink so no source emit
    // launches another one-shot. (g_inflight.launch() checks this — a launch
    // racing teardown either bails or is waited out by drain() below.)
    g_inflight.begin_shutdown();
    xi::TriggerBus::instance().clear_sink();
    if (g_continuous.load()) stop_dispatch_pool_();   // joins workers + timer + drains lanes
    // Drain in-flight detached cmd:run / one-shot threads. A bare g_run_mu acquire
    // only waits for a thread that already HOLDS the lock; one detached-but-not-yet-
    // locked would slip past and then touch the about-to-be-destroyed srv. drain()
    // waits on the in-flight count (capped) instead.
    //
    // T1: if drain() TIMES OUT (a wedged inspect — infinite loop / blocking plugin —
    // still in flight after the cap), we must NOT proceed into teardown: close_project
    // below FreeLibrary's the plugin DLLs and g_srv_for_bp is nulled while that thread
    // is still inside run_one_inspection(*srv)/process_fn_ → UAF / access-violation.
    // With the watchdog DISABLED (default g_watchdog_ms{0}) nothing else would have
    // killed the process, so this path is reachable. Do exactly what the watchdog's
    // HARD-trip does: log, flush, and std::_Exit(WATCHDOG_EXIT_CODE) — a crash-safe
    // hard exit (skips static dtors / atexit a wedged worker could deadlock) that the
    // FE supervisor respawns. A clean teardown here is unsafe precisely BECAUSE a
    // thread is wedged; the hard exit is the correct trade.
    if (!g_inflight.drain()) {
        int stuck = g_inflight.inflight();
        std::fprintf(stderr,
            "[xinsp2] shutdown drain TIMED OUT with %d wedged in-flight inspect(s) — "
            "hard-exiting instead of tearing down (would UAF: FreeLibrary + srv destroy "
            "under a live detached run); FE supervisor respawns (rc=0x%04X)\n",
            stuck, WATCHDOG_EXIT_CODE);
        if (auto* s = g_srv_for_bp.load(std::memory_order_acquire))
            emit_error_log(*s,
                "shutdown drain timed out with " + std::to_string(stuck) +
                " wedged in-flight inspect(s); backend hard-exiting for respawn");
        std::fflush(stderr);
        std::fflush(stdout);
        std::_Exit(WATCHDOG_EXIT_CODE);
    }
    { std::lock_guard<std::mutex> rl(g_run_mu); }     // belt-and-suspenders
    xi::TriggerBus::instance().reset();               // prune the per-source emit-time map (source names go out of scope here)
    { std::lock_guard<std::mutex> lk(g_script_mu); xi::script::unload_script(g_script); }
    // Close the open project (if any) NOW — while the ImagePool singleton is still
    // alive — so plugin instances are destroyed in the correct order (instances first,
    // THEN FreeLibrary) and their image-handle sweep runs against a live pool. The
    // normal exit paths (cmd:shutdown, g_should_exit epilogue) otherwise never called
    // close_project, leaving ~PluginManager to do it at static destruction — after
    // FreeLibrary (destroy_fn into unmapped code) and after ImagePool was torn down
    // (release_all_for on a destroyed singleton). Idempotent: no-op if already closed.
    g_plugin_mgr.close_project();
    g_srv_for_bp = nullptr;                            // last: every emitter is quiesced now
    g_teardown_done.store(true);                       // T2: unblock a waiting console handler
}

// T2 — orderly shutdown on an abrupt console exit (window close, Ctrl+C/Break,
// logoff, system shutdown), which otherwise bypasses controlled_shutdown_teardown_
// entirely (the OS default handler ExitProcess'es: no plugin destructors, so a
// comm/PLC plugin's "go-safe on close" never fires, and the still-armed crash
// filter can turn the kill into a spurious minidump the FE reads as a crash).
// The handler just flips g_should_exit — the main serve loop polls it every 100ms
// and runs the SAME controlled_shutdown_teardown_ path. For the close-class events
// the OS force-terminates shortly after the handler returns, so we BLOCK (bounded)
// until teardown signals done, keeping the process alive long enough to tear down
// cleanly in the common (no-wedge) case. Ctrl+C/Break have no hard deadline.
// Registered only in main() (the backend exe) — never affects embedded/SDK use.
#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler_(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: {
            std::fprintf(stderr, "[xinsp2] console control event %lu — clean shutdown\n",
                         (unsigned long)type);
            std::fflush(stderr);
            g_should_exit.store(true);
            const bool close_class = (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT);
            if (close_class) {
                // ~4.5s budget (< the OS's default ~5s CTRL_CLOSE window) for main()
                // to run teardown. If teardown hard-exits on a wedged drain (T1), the
                // process is already gone; this loop just falls through on timeout.
                for (int i = 0; i < 450 && !g_teardown_done.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return TRUE;   // handled — suppress the default terminate
        }
        default:
            return FALSE;
    }
}
#endif

// Install the bus sink so triggers always dispatch: in continuous mode they feed
// the worker-pool queue; otherwise each trigger runs a single-shot inspect. This
// is installed on every compile_and_load so "issue"/"replay" works WITHOUT needing
// cmd:start — the trigger-driven model (continuous is just an optional free-running
// timer on top).
static void install_trigger_sink_(xi::ws::Server* srv) {
    // B1: apply the project's one-shot in-flight ceiling. Installed on every
    // compile_and_load, so a project's parallelism.max_inflight takes effect
    // WITHOUT needing cmd:start (one-shot dispatch works pre-start). <=0 → default.
    g_inflight.set_cap(g_plugin_mgr.project().max_inflight);
    xi::TriggerBus::instance().set_sink([srv](xi::TriggerEvent ev) {
        if (g_continuous.load()) {
            // Route by the emitting source instance's "group" (default_group if
            // the source is untagged/unknown, or the synthetic timer tick). A
            // project with no explicit groups resolves to the synthesized default
            // lane (group "") — see spawn_group_pool_. instance_group() does the
            // lookup UNDER PluginManager's lock — this sink runs on a source's emit
            // thread, concurrent with create/remove/rename_instance.
            ev.group = g_plugin_mgr.instance_group(ev.leader_source);
            (void)enqueue_to_lane_(std::move(ev));
        } else {
            dispatch_one_shot_(srv, std::move(ev));
        }
    });

}

// Quiesce the dispatcher + timer before any handler
// that's about to touch the script DLL or project plugin DLLs (audit
// P0-AB-1..5). Returns the prior continuous state so the caller can
// re-arm if it wants. Mirrors the inline block compile_and_load has
// used since FL r5.
//
// IMPORTANT: this only quiesces the bus-driven dispatcher pool. Any
// detached cmd:run threads are not joined here — those snapshot the
// script under g_script_mu and the destructive caller still holds
// g_script_mu (or equivalent) while the inspect runs to completion.
// For project plugin DLLs (touched by close/open/recompile/export),
// no equivalent detached path exists; the dispatch pool is the only
// in-flight surface.
// RAII: stop continuous dispatch for a lifecycle op (DLL unload/reload/commit) and
// RESUME it when the guard goes out of scope. Resume-on-destruction is the default
// so "quiesce must be symmetric with resume" is guaranteed by the type, not by each
// handler remembering to respawn — five handlers (recompile/rebuild/commit/discard/
// export) used to quiesce and never resume, silently stopping the live stream on a
// hot-recompile. An op that legitimately should NOT resume (the stream ends or is
// replaced: unload_script / close_project / open_project) calls dismiss().
// MOVE-ONLY: quiesce_dispatch_for_lifecycle_op_ returns one by value; a moved-from
// guard won't resume. CALLERS MUST HOLD IT (`auto g = quiesce_...`) for the op's
// duration — a discarded temporary would resume immediately, before the op runs.
struct DispatchPoolGuard {
    bool            was_continuous = false;
    int             prior_fps = 10;
    bool            quiesced = false;
    xi::ws::Server* srv = nullptr;
    bool            armed_ = true;   // false once resumed/dismissed/moved-from
    bool            paused_launches_ = false;  // we g_inflight.pause()'d — dtor MUST unpause
    bool            restore_sink_    = false;  // we cleared the bus sink — resume re-installs it

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

    // Op done, stream continues: re-enable detached launches, re-install the one-shot
    // sink, and respawn continuous mode at the prior fps if we quiesced it. Idempotent.
    void resume() {
        if (!armed_) return;
        armed_ = false;
        // ALWAYS re-enable launches (even a non-continuous project needs one-shots
        // for issue/replay) and restore the sink we cleared.
        if (paused_launches_) { g_inflight.unpause(); paused_launches_ = false; }
        if (restore_sink_ && srv) { install_trigger_sink_(srv); restore_sink_ = false; }
        if (was_continuous && quiesced) {
            bool trig_only = prior_fps <= 0;
            g_continuous_fps = trig_only ? 0 : prior_fps;
            g_continuous = true;
            int interval_ms = trig_only ? 0 : std::max(1, 1000 / std::max(prior_fps, 1));
            spawn_group_pool_(srv, interval_ms);
            std::fprintf(stderr, "[xinsp2] continuous mode resumed\n");
        }
    }
    // The op intentionally leaves dispatch stopped (the stream ended or is replaced:
    // unload_script / close_project / open_project). Do NOT re-install the sink or
    // respawn continuous — BUT still re-enable launches (the next project needs them).
    void dismiss() {
        armed_ = false;
        if (paused_launches_) { g_inflight.unpause(); paused_launches_ = false; }
    }
};

static DispatchPoolGuard quiesce_dispatch_for_lifecycle_op_(const char* op_name,
                                                            xi::ws::Server* srv) {
    DispatchPoolGuard g;
    g.srv = srv;
    // Stop NEW detached one-shot / cmd:run launches and drop the bus sink BEFORE the
    // op FreeLibrary's any plugin DLL. A one-shot dispatch runs on a SOURCE plugin's
    // own emit thread (bus sink -> dispatch_one_shot_ -> g_inflight.launch), NOT this
    // handler thread — so without this a source emitting mid-op could launch an
    // inspect that calls into a DLL being unloaded (use-after-unload). clear_sink stops
    // future fires; pause()+drain() is the Dekker handshake that also catches an emit
    // already past the sink read but not yet counted. The guard reverses both (resume
    // re-installs the sink + unpauses; dismiss unpauses without re-installing).
    g_inflight.pause();
    g.paused_launches_ = true;
    g.restore_sink_    = xi::TriggerBus::instance().has_sink();   // only restore if one existed
    xi::TriggerBus::instance().clear_sink();
    if (g_continuous.load()) {
        g.was_continuous = true;
        g.prior_fps = g_continuous_fps.load();
        stop_dispatch_pool_();
        g.quiesced = true;
        std::fprintf(stderr,
            "[xinsp2] stopped continuous mode for %s (resumes when the op completes)\n",
            op_name);
    }
    // (Lane queues are drained + their image handles released inside
    // stop_dispatch_pool_ -> stop_group_pool_ above, before the DLLs unload.)
    // Wait out any in-flight detached cmd:run / one-shot inspect already running:
    // they hold g_run_mu for the whole inspect and call into the plugin/script DLLs
    // this op is about to FreeLibrary. drain() waits on the in-flight count (paused
    // above so none can start meanwhile); the g_run_mu acquire is belt-and-suspenders.
    g_inflight.drain();
    { std::lock_guard<std::mutex> lk(g_run_mu); }
    return g;
}

// ---------------------------------------------------------------------------
// Host-tracked instance state (orchestrator get_state — task #67).
//
// A deliberately tiny state machine the HOST keeps per instance, driven by the
// host-visible orchestrator verbs. Fine staging/ready sub-state stays plugin-
// side (exchange get_status); the host records only these three coarse states +
// the last error. In the exchange-convention prototype the host can't see
// prepare/commit (they ride opaque exchange commands), so only create_instance,
// set_instance_def and commit_group move the state — once prepare/commit are
// first-class ABI (task #69) the host will observe them directly and refine this.
// ---------------------------------------------------------------------------
// The state map itself now lives in the PluginManager, OWNED alongside the
// instance set under its one mutex, so create/remove/rename migrate it atomically
// and it can never drift out of sync (the bug class the earlier rounds kept
// hitting). These free functions are thin pass-throughs kept only so the call
// sites below read the same as before. drop_inst_state / rename_inst_state are
// GONE on purpose: remove_instance / rename_instance migrate the state inline, so
// the WS handlers must NOT do it separately.
using xi::InstState;

static const char* inst_state_str(InstState s) {
    switch (s) { case InstState::Active: return "active";
                 case InstState::Faulted: return "faulted";
                 default: return "created"; }
}

static void set_inst_state(const std::string& name, InstState s,
                           const std::string& err = "") {
    g_plugin_mgr.set_instance_state(name, s, err);
}
static void clear_inst_state() {
    g_plugin_mgr.clear_instance_states();
}

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

    if (name == "ping") {
        char buf[128];
        std::snprintf(buf, sizeof(buf), R"({"pong":true,"ts":%.3f})", now_seconds());
        send_rsp_ok(srv, id, buf);
    } else if (name == "version") {
        std::string vd = std::string(R"({"version":")") + XINSP2_VERSION
                       + R"(","commit":")" + XINSP2_COMMIT
                       + R"(","abi":1})";
        send_rsp_ok(srv, id, vd);
    } else if (name == "crash_reports") {
        // List crash JSON reports left by previous fatal crashes.
        // Returns the file contents inline (each is small, KB-sized).
        namespace fs = std::filesystem;
        auto dir = fs::temp_directory_path() / "xinsp2" / "crashdumps";
        std::string out = "{\"reports\":[";
        bool first = true;
        std::error_code ec;
        if (fs::exists(dir, ec)) {
            std::vector<fs::directory_entry> entries;
            for (auto& e : fs::directory_iterator(dir, ec)) {
                if (e.path().extension() == ".json") entries.push_back(e);
            }
            // Sort newest-first by mtime
            std::sort(entries.begin(), entries.end(),
                [](auto& a, auto& b) {
                    std::error_code ec2;
                    return fs::last_write_time(a.path(), ec2) > fs::last_write_time(b.path(), ec2);
                });
            for (auto& e : entries) {
                std::ifstream f(e.path(), std::ios::binary);
                std::stringstream ss; ss << f.rdbuf();
                std::string body = ss.str();
                while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
                if (body.empty() || body[0] != '{') continue;
                if (!first) out += ",";
                first = false;
                out += "{\"file\":";
                xp::json_escape_into(out, e.path().filename().string());
                out += ",\"report\":";
                out += body;
                out += "}";
            }
        }
        out += "]}";
        send_rsp_ok(srv, id, out);
    } else if (name == "clear_crash_reports") {
        namespace fs = std::filesystem;
        auto dir = fs::temp_directory_path() / "xinsp2" / "crashdumps";
        int n = 0;
        std::error_code ec;
        if (fs::exists(dir, ec)) {
            for (auto& e : fs::directory_iterator(dir, ec)) {
                fs::remove(e.path(), ec);
                ++n;
            }
        }
        send_rsp_ok(srv, id, "{\"removed\":" + std::to_string(n) + "}");
    } else if (name == "set_watchdog_ms") {
        // P2.4. Set the wall-clock budget (ms) for a single inspect()
        // call. 0 disables. Tripping the watchdog does not auto-reset —
        // the next inspect re-arms with the new budget.
        auto ms_opt = xp::get_number_field(parsed->args_json, "ms");
        int ms = ms_opt ? (int)*ms_opt : 0;
        if (ms < 0) ms = 0;
        if (ms > 600000) ms = 600000;     // 10-minute hard cap
        g_watchdog_ms = ms;
        std::string out = "{\"ms\":" + std::to_string(ms);
        out += ",\"trips\":" + std::to_string(g_watchdog_trips.load()) + "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "set_process_priority") {
        // Live process priority (Win). class: high|above|normal|below|realtime.
        // Mirrors --priority / project.json runtime.process_priority.
        auto c = xp::get_string_field(parsed->args_json, "class");
        std::string cls = c ? *c : "";
        if (apply_process_priority_(cls)) {
            send_rsp_ok(srv, id, "{\"process_priority\":\"" + cls + "\"}");
        } else {
            send_rsp_err(srv, id, "bad priority class (high|above|normal|below|realtime)");
        }
    } else if (name == "set_timer_fps") {
        // Live synthetic-tick rate. fps <= 0 = trigger-only (no ticks). Takes
        // effect on the next timer loop while continuous mode is running; persisted
        // by the UI to project.json runtime.timer_fps.
        auto f = xp::get_number_field(parsed->args_json, "fps");
        int fps = f ? (int)*f : 0;
        // max(1,..) so a high fps (>1000) doesn't round to 0, which the timer
        // loop reads as "off" (the opposite of what was asked). fps<=0 = off.
        int iv = fps > 0 ? std::max(1, 1000 / fps) : 0;
        g_timer_interval_ms.store(iv);
        std::string out = "{\"fps\":" + std::to_string(fps) +
                          ",\"interval_ms\":" + std::to_string(iv) + "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "watchdog_status") {
        std::string out = "{\"ms\":" + std::to_string(g_watchdog_ms.load());
        out += ",\"trips\":" + std::to_string(g_watchdog_trips.load());
        out += ",\"armed\":";
        // armed == at least one inspect slot is currently in flight.
        bool armed = false;
        for (int i = 0; i < WD_SLOTS; ++i) if (g_wd_deadlines[i].load() != 0) { armed = true; break; }
        out += (armed ? "true" : "false");
        out += "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "graph_capture") {
        // Toggle pipeline-graph dataflow capture (stage 2). Default off → no
        // hot-path cost. Enabling clears any prior recording.
        bool enable = parsed->args_json.find("\"enable\":true")  != std::string::npos ||
                      parsed->args_json.find("\"enable\": true") != std::string::npos;
        xi::GraphCapture::instance().set(enable);
        send_rsp_ok(srv, id, std::string("{\"capturing\":") + (enable ? "true" : "false") + "}");
    } else if (name == "graph_snapshot") {
        // Reconstruct dataflow EDGES (xi::GraphCapture owns the algorithm) and
        // format the result to wire JSON. Returns the instances that actually
        // ran + the edges.
        auto snap = xi::GraphCapture::instance().snapshot();
        std::string out = "{\"capturing\":";
        out += snap.capturing ? "true" : "false";
        out += ",\"ran\":[";
        for (size_t i = 0; i < snap.ran.size(); ++i) {
            if (i) out += ",";
            out += xp::json_escape(snap.ran[i]);     // json_escape() already wraps in quotes
        }
        out += "],\"edges\":[";
        bool first = true;
        for (auto& e : snap.edges) {
            if (!first) out += ","; first = false;
            out += "{\"from\":" + xp::json_escape(e.from) +
                   ",\"to\":"   + xp::json_escape(e.to) + ",\"keys\":[";
            bool k1 = true;
            for (auto& k : e.keys) { if (!k1) out += ","; k1 = false; out += xp::json_escape(k); }
            out += "]}";
        }
        out += "]}";
        send_rsp_ok(srv, id, out);
    } else if (name == "shutdown") {
        // Controlled teardown while everything is still alive, so nothing runs a
        // bus emit / module_lifetime deleter against a half-destroyed process at
        // static-destruction time. Single source of truth (see the function).
        controlled_shutdown_teardown_();
        send_rsp_ok(srv, id);
        g_should_exit = true;
    } else if (name == "compile_and_load") {
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
        if (g_continuous.load()) {
            was_continuous = true;
            prior_continuous_fps = g_continuous_fps.load();
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
            g_continuous_fps = fps;
            g_continuous = true;
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
            if (p.is_relative() && !g_project_folder.empty()) p = std::filesystem::path(g_project_folder) / p;
            // Containment: a prebuilt DLL MUST resolve inside the open project
            // folder. Without this an absolute/UNC `.dll` path loads an
            // arbitrary DLL — its DllMain / static initializers execute
            // in-process — and the WS port is unauthenticated by default.
            // Legitimate AOT bundles ship the DLL inside the project and
            // reference it relatively (already resolved above), so this only
            // rejects out-of-tree paths.
            std::error_code ec1, ec2;
            auto canon_dll  = std::filesystem::weakly_canonical(p, ec1);
            auto canon_proj = g_project_folder.empty()
                ? std::filesystem::path{}
                : std::filesystem::weakly_canonical(std::filesystem::path(g_project_folder), ec2);
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
        req.output_dir      = (std::filesystem::path(g_work_dir) / "script_build").string();
        req.include_dir     = g_include_dir;
        req.opencv_dir      = g_opencv_dir;
        req.turbojpeg_root  = g_turbojpeg_root;
        req.ipp_root        = g_ipp_root;
        req.vcvars_path     = g_tc_vcvars;   // empty = compiler auto-finds vcvars64.bat
        // Project-declared external deps (project.json include_dirs / link_libs).
        read_script_deps_(g_project_folder, req.include_dirs, req.link_libs, req.openmp_max_threads);
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
            // DLL (it holds g_run_mu for the whole inspect and runs from the old
            // module). Order is g_run_mu -> g_script_mu, matching the run path.
            std::lock_guard<std::mutex> rl(g_run_mu);
            std::lock_guard<std::mutex> lk(g_script_mu);
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
                resume_continuous_if_needed();   // old g_script untouched, keep it streaming
                return;
            }
            // Save persistent state from the OLD DLL before swapping it out.
            // Stamp the OLD DLL's schema version alongside so restore into the
            // new DLL can detect a shape mismatch.
            if (g_script.ok() && g_script.get_state) {
                std::vector<char> buf(256 * 1024);
                int n = g_script.get_state(buf.data(), (int)buf.size());
                if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024);
                             n = g_script.get_state(buf.data(), (int)buf.size()); }
                if (n > 0) g_persistent_state_json.assign(buf.data(), (size_t)n);
                g_persistent_state_schema = g_script.state_schema_version
                                          ? g_script.state_schema_version()
                                          : 0;
            }
            // Swap: move-assign drops the old module's last ref — its deleter
            // does the owner-sweep + FreeLibrary once any in-flight inspect that
            // snapshotted it returns.
            g_script = std::move(next);
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
            if (g_script.set_use_callbacks) {
                g_script.set_use_callbacks(
                    (void*)use_process_cb,
                    (void*)use_exchange_cb,
                    (void*)use_grab_cb,
                    (void*)script_host_api_());
            }
            // Phase 3: trigger access callbacks. Older scripts that don't
            // import xi_script_set_trigger_callbacks just stay null and
            // xi::current_trigger().is_active() returns false.
            if (g_script.set_trigger_callbacks) {
                g_script.set_trigger_callbacks(
                    (void*)trigger_info_cb,
                    (void*)trigger_image_cb,
                    (void*)trigger_sources_cb);
            }
            // Optional newer symbol — scripts compiled before P2-2
            // simply don't export it and t.primary_source() falls back
            // to sources().front().
            if (g_script.set_trigger_leader_callback) {
                g_script.set_trigger_leader_callback((void*)trigger_leader_cb);
            }
            // ABI v5: metadata-doc callback. Scripts compiled before this symbol
            // existed simply don't export it and current_trigger().meta() returns
            // an empty Record.
            if (g_script.set_trigger_meta_callback) {
                g_script.set_trigger_meta_callback((void*)trigger_meta_cb);
            }
            // Status callback. Scripts without xi_status.hpp leave this null
            // and xi::status() is a no-op.
            if (g_script.set_status_callback) {
                g_script.set_status_callback((void*)status_cb);
            }
            // C1: image-pool owner get/set thunks. Lets xi::async / xi::parallel_for
            // carry the inspect-thread owner onto worker threads so their pool
            // images stay attributed (instead of anonymous owner=0). Optional
            // symbol — older scripts don't export it and propagation is a no-op.
            if (g_script.set_owner_callbacks) {
                g_script.set_owner_callbacks((void*)owner_get_cb, (void*)owner_set_cb);
            }
            // Result callback. Scripts without xi_result.hpp leave this null
            // and xi::result() is a no-op (run_result then defaults to NA).
            if (g_script.set_result_callback) {
                g_script.set_result_callback((void*)result_cb);
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
            if (g_script.set_param) {
                for (auto& [pname, pval] : g_param_cache) {
                    g_script.set_param(pname.c_str(), pval.c_str());
                }
                if (!g_param_cache.empty()) {
                    std::fprintf(stderr,
                        "[xinsp2] replayed %zu param value(s) into reloaded script\n",
                        g_param_cache.size());
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
            if (g_script.set_instance_def) {
                for (auto& [iname, def] : g_instance_def_cache) {
                    // Replaying a cached def enters the freshly-swapped DLL's plugin
                    // code while we hold g_run_mu + g_script_mu. A plugin that throws
                    // (or faults, via the SEH translator) on an old/incompatible cached
                    // def must NOT terminate the backend mid-swap — log + skip it and
                    // keep replaying the rest. Do NOT touch the held locks in the catch.
                    try {
                        g_script.set_instance_def(iname.c_str(), def.c_str());
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
                if (!g_instance_def_cache.empty()) {
                    std::fprintf(stderr,
                        "[xinsp2] replayed %zu instance def(s) into reloaded script\n",
                        g_instance_def_cache.size());
                }
            }

            // Restore persistent state into the new DLL — but drop it
            // when the schema versions disagree (and both sides
            // declared one), since set_state's silent default-fill on
            // a shape change would confuse the new code more than
            // starting fresh would.
            if (g_script.set_state && g_persistent_state_json.size() > 2) {
                int new_schema = g_script.state_schema_version
                               ? g_script.state_schema_version()
                               : 0;
                // Drop whenever the NEW script declares a schema version that
                // differs from the persisted one — INCLUDING the 0→N case (a
                // script adopting versioning for the first time): the old
                // unversioned shape would otherwise default-fill into the new
                // shape, silently mis-shaping state. new_schema==0 (a script that
                // doesn't version) keeps the legacy "best-effort restore" path.
                bool drop = (new_schema != 0 &&
                             g_persistent_state_schema != new_schema);
                if (drop) {
                    // G4 / OQ-5: before dropping, give the NEW DLL a chance to
                    // migrate the prior state forward via its opt-in code_change
                    // hook. If it carries state across the schema change, restore
                    // the migrated shape instead of dropping. Absent hook (or a
                    // decline) falls through to the unchanged drop path below.
                    std::string migrated;
                    if (xi::script::migrate_state(g_script, g_persistent_state_json,
                                                  g_persistent_state_schema, new_schema,
                                                  migrated)) {
                        // set_state enters plugin code under g_run_mu + g_script_mu;
                        // a throwing/faulting plugin must not terminate the swap. On
                        // failure, drop the migrated state and continue (no lock touch).
                        bool state_ok = true;
                        try {
                            g_script.set_state(migrated.c_str());
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
                        if (!state_ok) { g_persistent_state_json = "{}"; }
                        else {
                        g_persistent_state_json = migrated;
                        std::fprintf(stderr,
                            "[xinsp2] state schema changed (v%d → v%d) — migrated prior state "
                            "(%zu bytes) via code_change hook\n",
                            g_persistent_state_schema, new_schema, migrated.size());
                        std::string ev = "{\"type\":\"event\",\"name\":\"state_migrated\","
                                         "\"data\":{\"old_schema\":"
                                       + std::to_string(g_persistent_state_schema)
                                       + ",\"new_schema\":"
                                       + std::to_string(new_schema)
                                       + "}}";
                        srv.send_text(ev);
                        g_persistent_state_schema = new_schema;
                        }
                    } else {
                    std::fprintf(stderr,
                        "[xinsp2] state schema changed (v%d → v%d) — dropping prior state\n",
                        g_persistent_state_schema, new_schema);
                    std::string ev = "{\"type\":\"event\",\"name\":\"state_dropped\","
                                     "\"data\":{\"old_schema\":"
                                   + std::to_string(g_persistent_state_schema)
                                   + ",\"new_schema\":"
                                   + std::to_string(new_schema)
                                   + "}}";
                    srv.send_text(ev);
                    g_persistent_state_json = "{}";
                    }
                } else {
                    // set_state enters plugin code under g_run_mu + g_script_mu; a
                    // throwing/faulting plugin must not terminate the swap — log + skip.
                    try {
                        g_script.set_state(g_persistent_state_json.c_str());
                        std::fprintf(stderr, "[xinsp2] state restored (%zu bytes, schema v%d)\n",
                                     g_persistent_state_json.size(), new_schema);
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
    } else if (name == "unload_script") {
        // P0-AB-1: dispatcher workers snapshot g_script under
        // g_script_mu and may be mid-inspect when unload_script
        // FreeLibrary's the DLL. Drain the pool first.
        { auto g = quiesce_dispatch_for_lifecycle_op_("unload_script", &srv); g.dismiss(); }  // script gone — don't resume
        std::lock_guard<std::mutex> lk(g_script_mu);
        xi::script::unload_script(g_script);
        // Drop the param replay cache — there's no live script to
        // replay into, and a future load_project / compile_and_load
        // is free to start clean.
        g_param_cache.clear();
        g_instance_def_cache.clear();   // sibling replay shadow — same lifetime
        send_rsp_ok(srv, id);
    } else if (name == "run") {
        if (g_continuous.load()) {
            send_rsp_err(srv, id, "cannot run while continuous mode is active — stop first");
            return;
        }
        // No script loaded: return a clear error NOW, before the ok+detached-run
        // path. `run` sends its rsp before vars, so without this a no-script run
        // would reply ok, then silently emit no vars — a headless driver waiting
        // for vars times out with an empty error and drops the WS. open_project
        // does not compile the project's script (that's compile_and_load's job),
        // so this is the common headless gotcha. (Reported bug BUG-3.)
        if (!g_script.ok()) {
            send_rsp_err(srv, id, "no script loaded — call compile_and_load first");
            return;
        }
        int64_t run_id = ++g_run_id;

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
        // Serialised on g_run_mu so 8 quick `cmd:run` calls produce
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
        // The detached thread dereferences the main-local srv, so g_inflight owns
        // the bump/bail/drain; teardown waits it out. A launch racing shutdown (or a
        // spawn failure) just runs nothing — there's no rsp for an async run anyway.
        g_inflight.launch([&srv, run_id,
                     frame_path = std::move(frame_path),
                     meta_json  = std::move(meta_json)]() {
            reserve_fault_stack();   // BUG 2: dump survives a script stack overflow
            xi::install_seh_translator();
            std::lock_guard<std::mutex> lk(g_run_mu);

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
                        xi::DocRegistry::instance().retain(meta);   // register at rc=1
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
    } else if (name == "start") {
        // Start continuous trigger mode. The backend runs a timer thread
        // that calls inspect() at a configurable interval. The script's
        // own ImageSource (if any) runs its acquisition thread inside
        // the DLL — the backend doesn't manage it.
        if (g_continuous.load()) {
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
        // g_timer_interval_ms already holds (project.json runtime.timer_fps, a prior
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
        if (g_timer_thread.joinable()) {
            stop_dispatch_pool_();
        }

        g_continuous_fps = trigger_only ? 0 : fps;
        g_continuous = true;

        // Seed the live timer rate (0 = trigger-only). Only when fps was explicit;
        // otherwise keep the existing g_timer_interval_ms (runtime/prior/default).
        if (fps_explicit) g_timer_interval_ms.store(trigger_only ? 0 : std::max(1, 1000 / std::max(fps, 1)));
        int interval_ms = g_timer_interval_ms.load();

        // Bus-driven dispatch: with g_continuous now true the sink enqueues to
        // the worker pool (single-shot otherwise). Timer thread emits synthetic
        // events on schedule for scripts without trigger sources.
        install_trigger_sink_(&srv);
        spawn_group_pool_(&srv, interval_ms);

        // The watchdog now tracks a per-inspect slot, so it protects every
        // worker under N>1 (no longer bypassed). On a hard trip the backend
        // exits for the FE to respawn; under N>1 the cooperative-cancel phase
        // is global (aborts all in-flight frames that round). See
        // run_one_inspection() + docs/guides/write-a-script.md.

        int n_threads = std::max(1, g_plugin_mgr.project().dispatch_threads);
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      R"({"started":true,"dispatch_threads":%d})", n_threads);
        send_rsp_ok(srv, id, buf);
    } else if (name == "stop") {
        g_continuous = false;
        xi::TriggerBus::instance().clear_sink();
        stop_dispatch_pool_();   // joins lanes + drains their queues (handles released)
        send_rsp_ok(srv, id, R"({"stopped":true})");
    } else if (name == "list_params") {
        // If a script is loaded, delegate to its own registry thunk so we
        // see the DLL's params. Otherwise report the backend's own.
        std::string params_json;
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.list_params) {
                std::vector<char> buf(64 * 1024);
                int n = g_script.list_params(buf.data(), (int)buf.size());
                if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024);  // widen: -INT_MIN is UB
                             n = g_script.list_params(buf.data(), (int)buf.size()); }
                if (n > 0) params_json.assign(buf.data(), (size_t)n);
            }
        }
        if (params_json.empty()) params_json = "[]";   // params live in the script DLL
        std::string out = "{\"type\":\"instances\",\"instances\":[],\"params\":";
        out += params_json;
        out += "}";
        send_rsp_ok(srv, id, "{}");
        srv.send_text(out);
    } else if (name == "set_param") {
        auto pname = xp::get_string_field(parsed->args_json, "name");
        if (!pname) {
            send_rsp_err(srv, id, "set_param: missing name");
            return;
        }
        // Extract the value's RAW JSON token and pass it verbatim to set_from_json.
        // The old path reformatted the number via "%g", which turned a big integer
        // (e.g. area_min=1000000) into "1e+06" -> std::stoll stops at 'e' -> the param
        // was SILENTLY set to 1; floats lost precision too (%g = 6 sig figs). It also
        // scanned the whole args for "value":true, which could false-match a string
        // value. find_key returns only the top-level value token (a number / true|false
        // / "quoted string"), exact and un-reformatted; the param's set_from_json
        // validates it (rc -2 if it rejects). A missing value isn't a missing param.
        std::string val;
        const char* after = nullptr;
        if (!xp::detail::find_key(parsed->args_json.data(),
                                  parsed->args_json.data() + parsed->args_json.size(),
                                  "value", val, after)) {
            send_rsp_err(srv, id, "set_param: missing 'value' for '" + *pname + "'");
            return;
        }
        // xi_script_set_param contract: 0 = set, -1 = no such param, -2 = the param
        // exists but rejected this value (set_from_json failed).
        int rc = 0; bool called = false;
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.set_param) {
                called = true;
                rc = g_script.set_param(pname->c_str(), val.c_str());
                // Cache an accepted value so compile_and_load replays it into the next
                // DLL load (else the new DLL's file-scope default silently overwrites it).
                if (rc == 0) g_param_cache[*pname] = val;
            }
        }
        if (!called) { send_rsp_err(srv, id, "set_param: no script loaded"); return; }
        if (rc == 0) { send_rsp_ok(srv, id); return; }
        if (rc == -1) { send_rsp_err(srv, id, std::string("no such param: ") + *pname); return; }
        send_rsp_err(srv, id, "set_param: '" + *pname + "' rejected the value (out of range / wrong type)");
    } else if (name == "list_instances") {
        std::string inst_json, params_json;
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok()) {
                std::vector<char> buf(64 * 1024);
                if (g_script.list_instances) {
                    int n = g_script.list_instances(buf.data(), (int)buf.size());
                    if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024); n = g_script.list_instances(buf.data(), (int)buf.size()); }
                    if (n > 0) inst_json.assign(buf.data(), (size_t)n);
                }
                if (g_script.list_params) {
                    int n = g_script.list_params(buf.data(), (int)buf.size());
                    if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024); n = g_script.list_params(buf.data(), (int)buf.size()); }
                    if (n > 0) params_json.assign(buf.data(), (size_t)n);
                }
            }
        }
        // Also include backend-managed instances (from PluginManager)
        auto& proj = g_plugin_mgr.project();
        std::string backend_inst = "[";
        int bi = 0;
        for (auto& [k, v] : proj.instances) {
            if (bi++) backend_inst += ",";
            backend_inst += "{\"name\":\"" + v.name + "\",\"plugin\":\"" + v.plugin_name + "\"}";
        }
        backend_inst += "]";

        // Merge: script instances + backend instances
        std::string merged_inst;
        if (!inst_json.empty() && inst_json != "[]" && bi > 0) {
            // Both have entries — merge arrays
            merged_inst = inst_json.substr(0, inst_json.size() - 1) + "," + backend_inst.substr(1);
        } else if (bi > 0) {
            merged_inst = backend_inst;
        } else {
            merged_inst = inst_json.empty() ? "[]" : inst_json;
        }

        std::string out = "{\"type\":\"instances\",\"instances\":";
        out += merged_inst;
        out += ",\"params\":";
        out += params_json.empty() ? "[]" : params_json;
        out += "}";
        send_rsp_ok(srv, id, "{}");
        srv.send_text(out);
    } else if (name == "set_instance_def") {
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        // Extract the def object as a raw JSON substring
        std::string def_str;
        const char* after;
        if (xp::detail::find_key(parsed->args_json.data(),
                                  parsed->args_json.data() + parsed->args_json.size(),
                                  "def", def_str, after)) {
            // def_str is the raw JSON value
        } else {
            def_str = "{}";
        }
        // Try backend's InstanceRegistry first (plugin-manager instances)
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        if (inst) {
            // set_def enters plugin code (C-ABI) — guard like exchange_instance so a
            // throwing/faulting plugin returns a clean error instead of terminating.
            try {
                if (inst->set_def(def_str)) {
                    set_inst_state(*iname, InstState::Active);
                    send_rsp_ok(srv, id);
                } else {
                    set_inst_state(*iname, InstState::Faulted, "set_def returned false");
                    send_rsp_err(srv, id, "set_def returned false");
                }
            } catch (const seh_exception& e) {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "set_def '%s' crashed: 0x%08X (%s)",
                             iname->c_str(), e.code, e.what());
                set_inst_state(*iname, InstState::Faulted, msg);
                send_rsp_err(srv, id, msg);
            } catch (const std::exception& e) {
                std::string em = std::string("set_def error: ") + e.what();
                set_inst_state(*iname, InstState::Faulted, em);
                send_rsp_err(srv, id, em);
            }
        } else {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.set_instance_def) {
                try {
                    int rc = g_script.set_instance_def(iname->c_str(), def_str.c_str());
                    // Cache an accepted def so compile_and_load replays it into the next
                    // DLL load (else the new DLL's file-scope ctor silently reverts it).
                    if (rc == 0) g_instance_def_cache[*iname] = def_str;
                    if (rc == 0) { set_inst_state(*iname, InstState::Active); send_rsp_ok(srv, id); }
                    else { set_inst_state(*iname, InstState::Faulted, "set_instance_def failed");
                           send_rsp_err(srv, id, "set_instance_def failed"); }
                } catch (const seh_exception& e) {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg), "script set_instance_def '%s' crashed: 0x%08X (%s)",
                                 iname->c_str(), e.code, e.what());
                    set_inst_state(*iname, InstState::Faulted, msg);
                    send_rsp_err(srv, id, msg);
                } catch (const std::exception& e) {
                    std::string em = std::string("script set_instance_def error: ") + e.what();
                    set_inst_state(*iname, InstState::Faulted, em);
                    send_rsp_err(srv, id, em);
                }
            } else {
                send_rsp_err(srv, id, "instance not found: " + *iname);
            }
        }
    } else if (name == "get_instance_def") {
        // Symmetric read of set_instance_def: returns the instance's full def
        // JSON (incl. any assets the plugin round-trips, e.g. image_png_b64), so
        // a host can snapshot an instance without scraping exchange:get_status.
        // Loop over list_instances to snapshot a whole project (the foundation
        // for portable product/instrument config bundles).
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        // Backend's InstanceRegistry first (plugin-manager instances).
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        if (inst) {
            // get_def enters plugin code (C-ABI) — guard like exchange_instance.
            try {
                std::string def = inst->get_def();
                send_rsp_ok(srv, id, def.empty() ? "{}" : def);
            } catch (const seh_exception& e) {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "get_def '%s' crashed: 0x%08X (%s)",
                             iname->c_str(), e.code, e.what());
                send_rsp_err(srv, id, msg);
            } catch (const std::exception& e) {
                send_rsp_err(srv, id, std::string("get_def error: ") + e.what());
            }
        } else {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.get_instance_def) {
                try {
                    std::vector<char> buf(256 * 1024);
                    int n = g_script.get_instance_def(iname->c_str(), buf.data(), (int)buf.size());
                    if (n < 0 && n != -1) {   // -needed → grow + retry (-1 = not found)
                        buf.resize((size_t)(-(int64_t)n) + 1024);
                        n = g_script.get_instance_def(iname->c_str(), buf.data(), (int)buf.size());
                    }
                    if (n >= 0) send_rsp_ok(srv, id, std::string(buf.data(), (size_t)n));
                    else        send_rsp_err(srv, id, "instance not found: " + *iname);
                } catch (const seh_exception& e) {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg), "script get_instance_def '%s' crashed: 0x%08X (%s)",
                                 iname->c_str(), e.code, e.what());
                    send_rsp_err(srv, id, msg);
                } catch (const std::exception& e) {
                    send_rsp_err(srv, id, std::string("script get_instance_def error: ") + e.what());
                }
            } else {
                send_rsp_err(srv, id, "instance not found: " + *iname);
            }
        }
    } else if (name == "exchange_instance") {
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
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.exchange_instance) {
                try {
                    std::vector<char> rsp(256 * 1024);
                    int n = g_script.exchange_instance(iname->c_str(), cmd_str.c_str(),
                                                       rsp.data(), (int)rsp.size());
                    if (n < 0) { rsp.resize((size_t)(-(int64_t)n) + 1024);
                                 n = g_script.exchange_instance(iname->c_str(), cmd_str.c_str(),
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
    } else if (name == "get_state") {
        // Orchestrator read (task #67): the host-tracked instance state machine
        // (created / active / faulted) + last error. Coarse by design — fine
        // staging/ready sub-state is plugin-side (exchange get_status). An
        // instance that exists but has had no host-visible transition yet reads
        // "created". args: { "name": "cam0" } → data: { state, last_error }.
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        InstState st; std::string err; long long crashes = 0;
        bool known = g_plugin_mgr.get_instance_state(*iname, st, err, &crashes);
        if (!known) {
            // No tracked transition — report "created" if the instance actually
            // exists, else a clean not-found.
            if (xi::InstanceRegistry::instance().find(*iname)) { st = InstState::Created; }
            else { send_rsp_err(srv, id, "instance not found: " + *iname); return; }
        }
        std::string data = std::string("{\"state\":\"") + inst_state_str(st) + "\",\"last_error\":";
        xp::json_escape_into(data, err);
        // crash_count: a process() crash leaves the instance Active + returns NA, so
        // this is how a host detects a per-instance crash loop and alerts.
        data += ",\"crash_count\":" + std::to_string(crashes);
        data += "}";
        send_rsp_ok(srv, id, data);
    } else if (name == "prepare_instance") {
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
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.exchange_instance) {
                // Script-side prepare enters plugin code — guard like the backend
                // path above (and exchange_instance) so a throw/fault isn't fatal.
                try {
                    std::vector<char> buf(64 * 1024);
                    int n = g_script.exchange_instance(iname->c_str(), cmd.c_str(),
                                                       buf.data(), (int)buf.size());
                    if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024);
                                 n = g_script.exchange_instance(iname->c_str(), cmd.c_str(),
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
    } else if (name == "commit_group") {
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
            for (auto& [iname, ii] : g_plugin_mgr.project().instances) {
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
        // (pool stopped + workers joined + in-flight cmd:run drained via g_run_mu).
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
                std::lock_guard<std::mutex> lk(g_script_mu);
                if (g_script.ok() && g_script.exchange_instance) {
                    // Script-side commit enters plugin code — guard like the backend
                    // inst->commit() path above so a throw/fault isn't fatal (record
                    // it as a per-target failure and keep committing the rest).
                    try {
                        const char* commit_cmd = R"({"command":"commit"})";
                        std::vector<char> buf(64 * 1024);
                        int n = g_script.exchange_instance(targets[i].c_str(), commit_cmd,
                                                           buf.data(), (int)buf.size());
                        if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024);
                                     n = g_script.exchange_instance(targets[i].c_str(), commit_cmd,
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
    } else if (name == "save_project") {
        auto path = xp::get_string_field(parsed->args_json, "path");
        if (!path) { send_rsp_err(srv, id, "missing path"); return; }
        std::string params_json, inst_json;
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok()) {
                std::vector<char> buf(64 * 1024);
                if (g_script.list_params) {
                    int n = g_script.list_params(buf.data(), (int)buf.size());
                    if (n > 0) params_json.assign(buf.data(), (size_t)n);
                }
                if (g_script.list_instances) {
                    int n = g_script.list_instances(buf.data(), (int)buf.size());
                    if (n > 0) inst_json.assign(buf.data(), (size_t)n);
                }
            }
        }
        std::string content = xi::project::build_project_json(params_json, inst_json);
        if (xi::project::write_text(*path, content)) {
            send_rsp_ok(srv, id);
        } else {
            send_rsp_err(srv, id, "failed to write " + *path);
        }
    } else if (name == "commit_working_copy") {
        // Mirror the <project>/.xinsp_work scratch back onto the canonical
        // project — the UI "Save Project" action. Persist any live instance
        // configs to the scratch first so the commit captures them.
        // Drain the dispatch pool first (same constraint as discard_working_copy
        // / open_project): the commit reads instance configs + does a filesystem
        // mirror (add/overwrite/delete) on the scratch that continuous workers
        // are concurrently reading/writing.
        auto _wc_commit_guard = quiesce_dispatch_for_lifecycle_op_("commit_working_copy", &srv);  // resumes at block end
        std::string save_fail;
        for (auto& [iname, _] : g_plugin_mgr.project().instances) {
            if (!g_plugin_mgr.save_instance(iname)) save_fail = iname;
        }
        if (!save_fail.empty()) {
            // An instance config couldn't reach disk (disk full / read-only) — the
            // scratch we're about to commit is itself stale, so don't claim success.
            send_rsp_err(srv, id, "failed to persist instance '" + save_fail +
                         "' before commit (disk full / read-only?)");
        } else if (g_plugin_mgr.commit_working_copy()) {
            send_rsp_ok(srv, id, "{\"committed\":true,\"canonical\":" +
                        ([]{ std::string s; xp::json_escape_into(s, g_plugin_mgr.canonical_path()); return s; }()) + "}");
        } else {
            send_rsp_err(srv, id, "no working copy active (open with working_copy:true)");
        }
    } else if (name == "discard_working_copy") {
        // Blow away the scratch + re-seed from canonical, then reopen. Same
        // teardown constraint as open_project — drain the dispatch pool first.
        if (!g_plugin_mgr.has_working_copy()) {
            send_rsp_err(srv, id, "no working copy active");
            return;
        }
        auto _wc_discard_guard = quiesce_dispatch_for_lifecycle_op_("discard_working_copy", &srv);  // resumes at block end
        if (g_plugin_mgr.reopen_fresh_working_copy()) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "discard failed");
        }
    } else if (name == "load_project") {
        auto path = xp::get_string_field(parsed->args_json, "path");
        if (!path) { send_rsp_err(srv, id, "missing path"); return; }
        std::string content = xi::project::read_text(*path);
        if (content.empty()) { send_rsp_err(srv, id, "failed to read " + *path); return; }

        // Use yyjson to parse the project file properly
        yyjson_doc* doc = yyjson_read(content.c_str(), content.size(), 0);
        yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (!root) { send_rsp_err(srv, id, "invalid JSON in project file"); return; }

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
                    // Params live in the script DLL. Also write g_param_cache so a
                    // later compile_and_load replays THESE loaded values, not a stale
                    // pre-load cache — without this, editing the script + recompiling
                    // silently reverted every param to whatever was last set_param'd
                    // before load_project (the replay shadow had never been refreshed).
                    std::lock_guard<std::mutex> lk(g_script_mu);
                    if (g_script.ok() && g_script.set_param) {
                        int rc = g_script.set_param(yyjson_get_str(nm), vbuf);
                        if (rc == 0) g_param_cache[yyjson_get_str(nm)] = vbuf;
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
        // backend registry first, then fall through to g_script.set_instance_def.
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
                        std::lock_guard<std::mutex> lk(g_script_mu);
                        if (g_script.ok() && g_script.set_instance_def) {
                            int rc = g_script.set_instance_def(iname, def_str);
                            // Mirror the param path above: write g_instance_def_cache so a
                            // later compile_and_load replays THIS recipe's def, not a stale
                            // pre-load value — else editing the script + recompiling reverts
                            // the just-loaded instance to the source default.
                            if (rc == 0) g_instance_def_cache[iname] = def_str;
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
        // Succeeded-with-warnings: the project loaded, but surface any params that
        // didn't apply so the caller can tell the operator the recipe was only
        // partially restored (instead of a silent clean ok).
        if (param_warnings.empty() && instance_warnings.empty()) {
            send_rsp_ok(srv, id);
        } else {
            std::string data = "{\"param_warnings\":[";
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
            send_rsp_ok(srv, id, data);
        }
    } else if (name == "list_plugins") {
        auto plugins = g_plugin_mgr.list_plugins();
        std::string out = "[";
        auto esc = [](const std::string& s) {
            std::string o; for (char c : s) { if (c=='\\'||c=='"') o.push_back('\\'); o.push_back(c); } return o;
        };
        for (size_t i = 0; i < plugins.size(); ++i) {
            if (i) out += ",";
            auto& p = plugins[i];
            out += "{\"name\":\"" + esc(p.name) + "\",\"description\":\"" + esc(p.description) + "\"";
            out += ",\"folder\":\"" + esc(p.folder_path) + "\"";
            out += ",\"has_ui\":" + std::string(p.has_ui ? "true" : "false");
            out += ",\"loaded\":" + std::string(p.handle ? "true" : "false");
            // cmake/prebuilt plugins get the per-item "Rebuild" action in the
            // extension's Plugin Browser (rebuild_plugins {plugins:[name]}).
            out += ",\"prebuilt\":" + std::string(p.prebuilt ? "true" : "false");
            // Same origin field as to_json — the extension's Plugin Browser relies
            // on it to badge project plugins, e2e journey asserts it.
            bool is_proj = g_plugin_mgr.is_project_plugin(p.name);
            out += ",\"origin\":\"" + std::string(is_proj ? "project" : "global") + "\"";
            // Optional `manifest` block from plugin.json (free-form;
            // see docs/reference/c-abi.md). AI agents and doc
            // tools read this to discover params / inputs / outputs /
            // exchange surface without grepping plugin source.
            if (!p.manifest_json.empty()) {
                out += ",\"manifest\":" + p.manifest_json;
            }
            out += "}";
        }
        out += "]";
        send_rsp_ok(srv, id, out);
    } else if (name == "recent_errors") {
        // Return error events captured by the cross-channel ring
        // (rsp.error / log level=error / async event etc).
        // Optional `since_ms` arg filters out older entries — useful
        // for "any errors since I sent my last cmd?" polling.
        auto since_opt = xp::get_number_field(parsed->args_json, "since_ms");
        int64_t since = since_opt ? (int64_t)*since_opt : 0;
        std::string out = "[";
        {
            std::lock_guard<std::mutex> lk(g_recent_errors_mu);
            int n = 0;
            for (auto& e : g_recent_errors) {
                if (e.ts_ms < since) continue;
                if (n++) out += ",";
                out += "{\"ts_ms\":" + std::to_string(e.ts_ms);
                out += ",\"source\":"; xp::json_escape_into(out, e.source);
                out += ",\"message\":"; xp::json_escape_into(out, e.message);
                if (e.cmd_id) out += ",\"cmd_id\":" + std::to_string(e.cmd_id);
                if (e.run_id) out += ",\"run_id\":" + std::to_string(e.run_id);
                out += "}";
            }
        }
        out += "]";
        send_rsp_ok(srv, id, out);
    } else if (name == "status") {
        // Snapshot of every component's latest sticky status. Clients call this
        // on EVERY (re)connect — that re-pull over the retained map is what
        // guarantees the latest status always arrives, even across reconnects
        // and backend respawns; the `status` push event is just a low-latency
        // accelerator between snapshots.
        std::string out = "{";
        {
            std::lock_guard<std::mutex> lk(g_status_mu);
            int n = 0;
            for (auto& [who, e] : g_status) {
                if (n++) out += ",";
                xp::json_escape_into(out, who);
                out += ":{\"text\":"; xp::json_escape_into(out, e.text);
                out += ",\"ts_ms\":" + std::to_string(e.ts_ms);
                out += ",\"seq\":" + std::to_string(e.seq) + "}";
            }
        }
        out += "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "image_pool_stats") {
        // Per-owner ImagePool footprint. Owner IDs alone are
        // meaningless to humans — we look them up against the
        // running project's instances + script so the reply names
        // who is holding the memory. Anonymous (owner == 0) is
        // collapsed under "label":"<host>".
        auto totals   = xi::ImagePool::instance().stats();
        auto by_owner = xi::ImagePool::instance().stats_by_owner();

        // Build owner_id → label map.
        std::unordered_map<xi::ImagePoolOwnerId, std::string> labels;
        labels[0] = "<host>";
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.owner_id != 0) {
                labels[g_script.owner_id] =
                    "script:" + std::filesystem::path(g_script.path).filename().string();
            }
        }
        for (auto& [iname, ii] : g_plugin_mgr.project().instances) {
            if (auto* a = dynamic_cast<xi::CAbiInstanceAdapter*>(ii.instance.get())) {
                labels[a->owner_id()] = "instance:" + ii.name + " (" + ii.plugin_name + ")";
            }
            // All instances are in-process CAbiInstanceAdapters now;
            // process-isolation + SHM were removed 2026-05.
        }

        auto label_for = [&](xi::ImagePoolOwnerId o) -> std::string {
            auto it = labels.find(o);
            if (it != labels.end()) return it->second;
            return "owner:" + std::to_string(o) + " (orphan)";
        };

        // Cumulative diagnostics: total_created and high_water never
        // decrement, so they expose activity even when live counts are
        // zero between runs (the agent feedback loop hit this — live
        // snapshots looked empty mid-test, hiding real allocation).
        auto cum = xi::ImagePool::instance().cumulative();
        std::string out = "{\"total\":{\"handles\":"
                        + std::to_string(totals.handle_count)
                        + ",\"bytes\":"
                        + std::to_string(totals.total_bytes)
                        + "},\"cumulative\":{\"total_created\":"
                        + std::to_string(cum.total_created)
                        + ",\"high_water\":"
                        + std::to_string(cum.high_water)
                        + ",\"live_now\":"
                        + std::to_string(cum.live_now)
                        + "},\"by_owner\":[";
        for (size_t i = 0; i < by_owner.size(); ++i) {
            if (i) out += ",";
            out += "{\"owner\":"   + std::to_string(by_owner[i].owner)
                +  ",\"label\":";
            xp::json_escape_into(out, label_for(by_owner[i].owner));
            out +=  ",\"handles\":" + std::to_string(by_owner[i].handle_count)
                +  ",\"bytes\":"    + std::to_string(by_owner[i].total_bytes)
                +  "}";
        }
        out += "]}";
        send_rsp_ok(srv, id, out);
    } else if (name == "rescan_plugins") {
        // Optional arg: {"dir": "<path>"} scans that one dir (additive).
        // No arg: re-scan the default plugins_dir.
        auto dir_opt = xp::get_string_field(parsed->args_json, "dir");
        const std::string& dir = dir_opt ? *dir_opt : g_plugins_dir;
        int n = 0;
        if (!dir.empty() && std::filesystem::exists(dir)) {
            n = g_plugin_mgr.scan_plugins(dir);
        }
        std::string out = "{\"scanned\":";
        xp::json_escape_into(out, dir);
        out += ",\"count\":" + std::to_string(n) + "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "unquarantine_plugin") {
        // Part III G2.3 — operator un-quarantine. Clears the G1 .xi_certify.json
        // verdict (crashed/quarantined) for a plugin so the next scan re-certifies
        // it from scratch. Accepts {"name": "<plugin>"} (resolved to its folder via
        // the last scan) or {"dir": "<folder>"}. Re-scans the default plugins dir
        // afterwards so a now-clean plugin is re-armed without a restart.
        auto pname = xp::get_string_field(parsed->args_json, "name");
        auto pdir  = xp::get_string_field(parsed->args_json, "dir");
        std::string key = pname ? *pname : (pdir ? *pdir : std::string());
        if (key.empty()) { send_rsp_err(srv, id, "missing name or dir"); return; }
        bool cleared = g_plugin_mgr.unquarantine_plugin(key);
        if (!cleared) { send_rsp_err(srv, id, "no quarantine found for: " + key); return; }
        int rearmed = 0;
        if (!g_plugins_dir.empty() && std::filesystem::exists(g_plugins_dir))
            rearmed = g_plugin_mgr.scan_plugins(g_plugins_dir);
        std::string out = "{\"unquarantined\":";
        xp::json_escape_into(out, key);
        out += ",\"rearmed\":" + std::to_string(rearmed) + "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "load_plugin") {
        auto pname = xp::get_string_field(parsed->args_json, "name");
        if (!pname) { send_rsp_err(srv, id, "missing name"); return; }
        if (g_plugin_mgr.load_plugin(*pname)) {
            send_rsp_ok(srv, id);
        } else {
            send_rsp_err(srv, id, "failed to load plugin: " + *pname);
        }
    } else if (name == "create_project") {
        auto folder = xp::get_string_field(parsed->args_json, "folder");
        auto pname  = xp::get_string_field(parsed->args_json, "name");
        if (!folder || !pname) { send_rsp_err(srv, id, "missing folder or name"); return; }
        if (g_plugin_mgr.create_project(*folder, *pname)) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "failed to create project");
        }
    } else if (name == "open_project") {
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
        // from the still-live old g_script, then (b) replay the prior project's
        // g_param_cache values over any same-named Param the new project
        // declares (e.g. "thresh") — running project B's inspections with
        // project A's tuned values / carried state and silently mis-verdicting.
        // A fresh project starts from its own file-scope defaults.
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            g_param_cache.clear();
            g_instance_def_cache.clear();   // sibling replay shadow — same project boundary
            g_persistent_state_json = "{}";
            g_persistent_state_schema = 0;
        }
        if (g_plugin_mgr.open_project(*folder, working_copy)) {
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
            auto& proj = g_plugin_mgr.project();
            int inst_count = (int)proj.instances.size();
            std::fprintf(stderr, "[xinsp2] project opened: %s (%d instances)\n",
                         proj.name.c_str(), inst_count);
            // Apply project.json "runtime" knobs. process_priority is live now;
            // timer_fps seeds the live timer rate (0 = trigger-only) for when
            // continuous mode runs.
            apply_process_priority_(proj.runtime_priority);
            if (proj.runtime_timer_fps >= 0)
                g_timer_interval_ms.store(proj.runtime_timer_fps > 0 ? std::max(1, 1000 / proj.runtime_timer_fps) : 0);
            for (auto& [k, v] : proj.instances) {
                std::fprintf(stderr, "[xinsp2]   instance: %s (%s)\n",
                             k.c_str(), v.plugin_name.c_str());
            }
            // Surface skip-bad-instance warnings to the user. The project
            // open still succeeds; bad instances are simply absent from
            // the runtime registry. Extension can show a toast.
            auto warns = g_plugin_mgr.open_warnings();
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
            g_project_folder = *folder;
            resolve_toolchain_(*folder);
            // Put the project folder on the DLL search path so a script's
            // statically-linked external dep DLL can live in the project folder.
            set_project_dll_search_(*folder);
            // (IDE IntelliSense config: the VS Code extension writes
            // <project>/.vscode/c_cpp_properties.json itself, reading the compile
            // paths via cmd:toolchain_health — the core no longer touches .vscode.)
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "failed to open project in " + *folder);
        }
    } else if (name == "close_project") {
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
        g_plugin_mgr.close_project();
        clear_inst_state();   // instances are gone — drop host-tracked state
        // Reset the script replay shadows on the PROJECT boundary, mirroring
        // unload_script's clear. Closing a project doesn't unload the script
        // DLL, but the operator-tuned param cache + persisted xi::state() belong
        // to the project just closed — leaving them in place would leak A's
        // values/state into whatever project is opened next (see open_project).
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            g_param_cache.clear();
            g_instance_def_cache.clear();   // sibling replay shadow — same project boundary
            g_persistent_state_json = "{}";
            g_persistent_state_schema = 0;
        }
        send_rsp_ok(srv, id, "{\"closed\":true}");
    } else if (name == "export_project_plugin") {
        // Package a project plugin as a deployable folder. Compiles Release;
        // the destination contains a self-contained plugin.json + DLL that can
        // be dropped into another project's plugins folder.
        auto pname = xp::get_string_field(parsed->args_json, "plugin");
        auto dest  = xp::get_string_field(parsed->args_json, "dest");
        if (!pname || !dest) { send_rsp_err(srv, id, "missing plugin or dest"); return; }
        if (!g_plugin_mgr.is_project_plugin(*pname)) {
            send_rsp_err(srv, id, "not a project plugin: " + *pname);
            return;
        }
        // export_project_plugin recompiles in Release; quiesce so no dispatcher
        // worker is mid-call into the same plugin's instances.
        auto _export_guard = quiesce_dispatch_for_lifecycle_op_("export_project_plugin", &srv);  // resumes at block end
        auto er = g_plugin_mgr.export_project_plugin(*pname, *dest);
        std::string data = "{\"plugin\":";
        xp::json_escape_into(data, *pname);
        data += ",\"dest\":";
        xp::json_escape_into(data, er.dest_dir);
        data += "}";
        if (er.ok) {
            send_rsp_ok(srv, id, data);
        } else {
            xp::Rsp r;
            r.id = id;
            r.ok = false;
            r.error = er.error;
            r.data_json = data;
            srv.send_text(r.to_json());
            if (!er.build_log.empty()) {
                xp::LogMsg lm;
                lm.level = "error";
                lm.msg = er.build_log;
                srv.send_text(lm.to_json());
            }
        }
    } else if (name == "recompile_project_plugin") {
        // Hot-rebuild a single project-local plugin. The extension calls
        // this from a file watcher when the user edits plugin source.
        // On success the plugin's instances are re-instantiated with
        // their previous defs intact; on failure the old DLL stays
        // loaded so running inspection isn't disrupted.
        auto pname = xp::get_string_field(parsed->args_json, "plugin");
        if (!pname) { send_rsp_err(srv, id, "missing plugin"); return; }
        if (!g_plugin_mgr.is_project_plugin(*pname)) {
            send_rsp_err(srv, id, "not a project plugin: " + *pname);
            return;
        }
        // P0-AB-4: recompile resets each instance pointer then
        // FreeLibrary's the old DLL. Any in-flight set_def / exchange
        // on those instances from a dispatcher worker would dereference
        // freed code. Drain first.
        auto guard = quiesce_dispatch_for_lifecycle_op_("recompile_project_plugin", &srv);
        auto rr = g_plugin_mgr.recompile_project_plugin(*pname);
        // Build diagnostics JSON — same shape as compile_and_load.
        std::string diag_json = "[";
        for (size_t i = 0; i < rr.diagnostics.size(); ++i) {
            auto& d = rr.diagnostics[i];
            if (i) diag_json += ",";
            diag_json += "{\"file\":";  xp::json_escape_into(diag_json, d.file);
            diag_json += ",\"line\":" + std::to_string(d.line);
            diag_json += ",\"col\":"  + std::to_string(d.col);
            diag_json += ",\"severity\":"; xp::json_escape_into(diag_json, d.severity);
            diag_json += ",\"code\":";    xp::json_escape_into(diag_json, d.code);
            diag_json += ",\"message\":"; xp::json_escape_into(diag_json, d.message);
            diag_json += "}";
        }
        diag_json += "]";
        std::string data = "{\"plugin\":";
        xp::json_escape_into(data, *pname);
        data += ",\"diagnostics\":" + diag_json;
        data += ",\"reattached\":[";
        for (size_t i = 0; i < rr.reattached_instances.size(); ++i) {
            if (i) data += ",";
            xp::json_escape_into(data, rr.reattached_instances[i]);
        }
        data += "]}";
        if (rr.ok) {
            send_rsp_ok(srv, id, data);
        } else {
            xp::Rsp r;
            r.id = id;
            r.ok = false;
            r.error = rr.error;
            r.data_json = data;
            srv.send_text(r.to_json());
            if (!rr.build_log.empty()) {
                xp::LogMsg lm;
                lm.level = "error";
                lm.msg = rr.build_log;
                srv.send_text(lm.to_json());
            }
        }
    } else if (name == "rebuild_plugins") {
        // `xInsp2: Rebuild Plugins`. For every cmake/prebuilt plugin whose source
        // changed, the backend unloads it (releasing the DLL file lock), runs its
        // own CMake build, then loads the rebuilt DLL and restores each instance's
        // def. Unchanged plugins (incl. CUDA/heavy-state ones you didn't touch)
        // are skipped. The unload→build→load ordering is why CMake runs host-side
        // (Windows can't overwrite a loaded DLL; CMake emits a fixed-name DLL).
        //
        // Optional args: {"cmake":"<path>", "config":"Release",
        //                 "plugins":["a","b"]}. `plugins` restricts the rebuild to
        // those names (the extension passes it to rebuild just what you're editing);
        // omitted = every cmake plugin.
        //
        // Same quiesce constraint as recompile: this resets instance pointers and
        // FreeLibrary's DLLs — drain dispatch first.
        auto cmake_exe = xp::get_string_field(parsed->args_json, "cmake");
        auto config    = xp::get_string_field(parsed->args_json, "config");
        std::vector<std::string> only;
        if (yyjson_doc* adoc = yyjson_read(parsed->args_json.c_str(), parsed->args_json.size(), 0)) {
            yyjson_val* arr = yyjson_obj_get(yyjson_doc_get_root(adoc), "plugins");
            if (yyjson_is_arr(arr)) {
                size_t _i, _n; yyjson_val* it;
                yyjson_arr_foreach(arr, _i, _n, it) {
                    const char* s = yyjson_get_str(it);
                    if (yyjson_is_str(it) && s) only.emplace_back(s);
                }
            }
            yyjson_doc_free(adoc);
        }
        auto guard = quiesce_dispatch_for_lifecycle_op_("rebuild_plugins", &srv);
        auto rep = g_plugin_mgr.rebuild_cmake_plugins(
            cmake_exe ? *cmake_exe : std::string("cmake"),
            config    ? *config    : std::string("Release"),
            only);
        std::string data = "{\"plugins\":[";
        bool any_fail = false;
        for (size_t i = 0; i < rep.items.size(); ++i) {
            auto& it = rep.items[i];
            if (i) data += ",";
            data += "{\"plugin\":"; xp::json_escape_into(data, it.name);
            data += ",\"status\":"; xp::json_escape_into(data, it.status);
            data += ",\"detail\":"; xp::json_escape_into(data, it.detail);
            data += "}";
            if (it.status == "failed") any_fail = true;
        }
        data += "]}";
        // Partial failures (failed[]) are still a completed run — return ok with
        // the per-plugin report; the client surfaces failures.
        send_rsp_ok(srv, id, data);
        if (any_fail)
            for (auto& it : rep.items)
                if (it.status == "failed") {
                    xp::LogMsg lm; lm.level = "error";
                    lm.msg = "rebuild_plugins: " + it.name + ": " + it.detail;
                    srv.send_text(lm.to_json());
                }
    } else if (name == "dispatch_stats") {
        // Snapshot of queue health. Useful for drivers / agents that
        // want to know if their source is overproducing.
        //
        // Field semantics:
        //   queue_depth_now             — current queue size
        //   queue_depth_cap             — configured project.json cap
        //   queue_depth_high_watermark  — peak depth observed since
        //                                 last cmd:start (real obs.)
        //   dropped_oldest / dropped_newest — overflow counters since
        //                                 last cmd:start
        //
        // ALL THREE COUNTERS (high_watermark, dropped_oldest,
        // dropped_newest) are reset by cmd:start. Drivers that snapshot
        // before AND after a start will see the AFTER values come back
        // smaller than BEFORE — don't subtract. See docs/reference/ws-protocol.md
        // `dispatch_stats` for the public contract.
        //
        // P1-8: `dropped_lifetime` and `queue_depth_high_watermark_lifetime` are the
        // process-uptime cumulatives — they do NOT reset at cmd:start, so a monitor
        // can answer "how much have we dropped total" / "peak depth ever" across run
        // boundaries (the per-run counters above answer "...since this start").
        std::string data;
        // Snapshot the lanes under g_lanes_mu so a concurrent stop can't free
        // them mid-read. A no-groups project has one synthesized default lane, so
        // the top-level totals are aggregates across all lanes (unified dispatch).
        std::vector<std::shared_ptr<GroupLane>> lanes;
        { std::lock_guard<std::mutex> lk(g_lanes_mu); lanes = g_lanes; }
        size_t qsz = 0; uint64_t hw = 0, dropped = 0;
        for (auto& lp : lanes) {
            { std::lock_guard<std::mutex> lk(lp->mu); qsz += lp->q.size(); }
            hw += lp->high_watermark.load();
            dropped += lp->dropped.load();
        }
        data  = "{\"queue_depth_now\":" + std::to_string(qsz);
        data += ",\"queue_depth_cap\":" + std::to_string(g_plugin_mgr.project().queue_depth);
        data += ",\"queue_depth_high_watermark\":" + std::to_string(hw);
        data += ",\"overflow\":\"" + g_plugin_mgr.project().overflow + "\"";
        data += ",\"dispatch_threads\":" + std::to_string(g_plugin_mgr.project().dispatch_threads);
        data += ",\"dropped\":" + std::to_string(dropped);
        // P1-8: process-uptime cumulatives (NOT reset by cmd:start).
        data += ",\"dropped_lifetime\":" + std::to_string(g_dropped_lifetime.load());
        data += ",\"queue_depth_high_watermark_lifetime\":" + std::to_string(g_high_watermark_lifetime.load());
        // Source liveness: ms since ANY source last emitted, + per-source ages. The
        // signal for "a camera stalled" — a stalled source otherwise stops the line
        // with zero indication. -1 = nothing has emitted yet. A monitor/FE applies a
        // source-rate-appropriate staleness threshold (auto-alerting on a fixed
        // threshold is left to the consumer since the expected rate is source-specific).
        {
            int64_t age_us = xi::TriggerBus::instance().last_emit_age_us();
            data += ",\"last_emit_age_ms\":" + (age_us < 0 ? std::string("-1")
                                              : std::to_string(age_us / 1000));
            auto ages = xi::TriggerBus::instance().source_emit_ages_us();
            data += ",\"sources\":[";
            for (size_t i = 0; i < ages.size(); ++i) {
                if (i) data += ",";
                data += "{\"source\":"; xp::json_escape_into(data, ages[i].first);
                data += ",\"last_emit_age_ms\":" + std::to_string(ages[i].second / 1000) + "}";
            }
            data += "]";
        }
        // Per-group lanes (always ≥1: the default lane when no groups declared).
        if (!lanes.empty()) {
            data += ",\"groups\":[";
            for (size_t i = 0; i < lanes.size(); ++i) {
                auto& l = *lanes[i];
                size_t lq; { std::lock_guard<std::mutex> lk(l.mu); lq = l.q.size(); }
                if (i) data += ",";
                data += "{\"name\":"; xp::json_escape_into(data, l.cfg.name);
                data += ",\"max_parallel\":" + std::to_string(l.cfg.max_parallel);
                data += ",\"thread_priority\":\"" + l.cfg.thread_priority + "\"";
                data += ",\"min_interval_ms\":" + std::to_string(l.cfg.min_interval_ms);
                data += ",\"queue_now\":" + std::to_string(lq);
                data += ",\"running\":" + std::to_string(l.running.load());
                data += ",\"high_watermark\":" + std::to_string(l.high_watermark.load());
                data += ",\"dropped\":" + std::to_string(l.dropped.load());
                data += ",\"cpu_affinity\":[";   // [] = unbound; else the per-worker mask sets
                for (size_t si = 0; si < l.cfg.cpu_affinity.size(); ++si) {
                    if (si) data += ",";
                    data += "[";
                    for (size_t ci = 0; ci < l.cfg.cpu_affinity[si].size(); ++ci) {
                        if (ci) data += ",";
                        data += std::to_string(l.cfg.cpu_affinity[si][ci]);
                    }
                    data += "]";
                }
                data += "]}";
            }
            data += "]";
        }
        data += "}";
        send_rsp_ok(srv, id, data);
    } else if (name == "metrics") {
        // OQ-7a observability export (core_fix_plan §21 / §27.5 "don't gold-plate").
        // The minimal, honest metrics surface: monotonic per-frame counters
        // (total/ok/error) + a fixed-bucket per-frame latency histogram, recorded
        // in run_one_inspection. Point-query snapshot over THIS existing WS channel
        // (same shape/role as dispatch_stats / image_pool_stats) — no separate
        // telemetry server. Counters are process-uptime cumulative (NOT reset by
        // cmd:start), so a monitor snapshots before/after and derives its own rates
        // (do not subtract across a restart — same caveat as dispatch_stats).
        std::string data;
        xi::MetricsRegistry::instance().snapshot_json(data);
        send_rsp_ok(srv, id, data);
    } else if (name == "open_project_warnings") {
        // Returns the per-instance warnings collected during the most
        // recent open_project. open_project itself succeeds even when
        // individual instances fail (skip-bad-instance), so this is
        // how a UI / agent surfaces those problems instead of having
        // to scrape backend stderr.
        auto warnings = g_plugin_mgr.open_warnings();
        std::string data = "{\"warnings\":[";
        bool first = true;
        for (auto& w : warnings) {
            if (!first) data += ",";
            first = false;
            data += "{\"instance\":";
            xp::json_escape_into(data, w.instance);
            data += ",\"plugin\":";
            xp::json_escape_into(data, w.plugin);
            data += ",\"reason\":";
            xp::json_escape_into(data, w.reason);
            data += "}";
        }
        data += "]}";
        send_rsp_ok(srv, id, data);
    } else if (name == "create_instance") {
        auto iname  = xp::get_string_field(parsed->args_json, "name");
        auto plugin = xp::get_string_field(parsed->args_json, "plugin");
        if (!iname || !plugin) { send_rsp_err(srv, id, "missing name or plugin"); return; }
        // Ensure plugin is loaded — surface WHY if it can't be (missing DLL,
        // missing factory symbol, ABI mismatch, etc.) instead of a generic failure.
        std::string load_err;
        if (!g_plugin_mgr.load_plugin(*plugin, &load_err)) {
            send_rsp_err(srv, id, load_err.empty() ? "failed to load plugin" : load_err);
            return;
        }
        // G2.1 — create() runs the plugin's factory (untrusted native code); stamp
        // the culprit so a factory fault is attributed to this plugin.
        stamp_culprit_(iname->c_str(), *plugin);
        std::string create_err;
        auto* ii = g_plugin_mgr.create_instance(*iname, *plugin, &create_err);
        if (ii) {
            // create_instance records the Created state internally (atomic with the
            // instance add) — no separate set_inst_state needed.
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, create_err.empty() ? "failed to create instance" : create_err);
        }
    } else if (name == "remove_instance") {
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        bool delete_folder =
            parsed->args_json.find("\"delete_folder\":true") != std::string::npos;
        if (g_plugin_mgr.remove_instance(*iname, delete_folder)) {
            // remove_instance drops the lifecycle state internally (atomic with the
            // unregister) — no separate drop_inst_state needed.
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "instance not found: " + *iname);
        }
    } else if (name == "rename_instance") {
        auto old_name = xp::get_string_field(parsed->args_json, "name");
        auto new_name = xp::get_string_field(parsed->args_json, "new_name");
        if (!old_name || !new_name) { send_rsp_err(srv, id, "missing name or new_name"); return; }
        using RR = xi::PluginManager::RenameResult;
        RR rr = g_plugin_mgr.rename_instance(*old_name, *new_name);
        if (rr == RR::Rejected) {
            send_rsp_err(srv, id, "rename failed — name in use or instance missing");
        } else {
            // Ok OR NotPersisted: the runtime + folder were renamed. rename_instance
            // already migrated the host-tracked state inside the same locked op, so
            // there's nothing to sync here — only the disk-save status differs.
            if (rr == RR::NotPersisted)
                send_rsp_err(srv, id, "renamed in memory but could not persist to disk "
                                      "(disk full / read-only?) — may revert on restart");
            else
                send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        }
    } else if (name == "get_project") {
        send_rsp_ok(srv, id, g_plugin_mgr.to_json());
    } else if (name == "save_instance_config") {
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        if (g_plugin_mgr.save_instance(*iname)) {
            send_rsp_ok(srv, id);
        } else {
            send_rsp_err(srv, id, "instance not found: " + *iname);
        }
    } else if (name == "get_plugin_ui") {
        // Return the path to the plugin's UI folder so the extension can
        // load it into a webview.
        auto plugin = xp::get_string_field(parsed->args_json, "plugin");
        if (!plugin) { send_rsp_err(srv, id, "missing plugin"); return; }
        auto* pi = g_plugin_mgr.find_plugin(*plugin);
        if (pi && pi->has_ui) {
            std::string data = "{\"ui_path\":";
            xp::json_escape_into(data, pi->ui_path);
            data += "}";
            send_rsp_ok(srv, id, data);
        } else {
            send_rsp_err(srv, id, "no UI for plugin: " + *plugin);
        }
    } else if (name == "get_dashboard") {
        // Serve the project's HMI dashboard so the HMI only ever needs the BE WS
        // URL (no filesystem coupling). Reads <project>/dashboard[.<name>].json.
        // args: { name?: string }. data: { found, name, dashboard:<verbatim JSON> }.
        auto nm = xp::get_string_field(parsed->args_json, "name");
        std::string fname = (nm && !nm->empty()) ? ("dashboard." + *nm + ".json") : "dashboard.json";
        // Guard the name against path escapes (only a simple token allowed).
        bool bad = fname.find("..") != std::string::npos || fname.find('/') != std::string::npos
                || fname.find('\\') != std::string::npos;
        std::string content;
        bool found = false;
        if (!bad && !g_project_folder.empty()) {
            std::ifstream f(std::filesystem::path(g_project_folder) / fname, std::ios::binary);
            if (f) { std::ostringstream ss; ss << f.rdbuf(); content = ss.str(); found = !content.empty(); }
        }
        std::string out = "{\"found\":" + std::string(found ? "true" : "false") + ",\"name\":";
        xp::json_escape_into(out, (nm && !nm->empty()) ? *nm : "");
        if (found) out += ",\"dashboard\":" + content;   // verbatim file (already JSON)
        out += "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "toolchain_health") {
        // C++ toolchain health check for the open project. Reports each
        // component's resolved path + source (override/env/default/none) +
        // whether it exists, so the config UI can warn on missing/wrong paths.
        send_rsp_ok(srv, id, toolchain_health_json_(g_project_folder));
    } else if (name == "set_toolchain_override") {
        // Pin (or clear) one toolchain path in the project's project.json
        // "toolchain" block. args: { key: "opencv"|"turbojpeg"|"ipp"|"vcvars"|
        // "include", path: "<dir-or-file>" }  (empty path clears the override).
        // Takes effect on the next compile; we also re-resolve + regenerate the
        // IntelliSense config immediately so the editor updates.
        auto key  = xp::get_string_field(parsed->args_json, "key");
        auto path = xp::get_string_field(parsed->args_json, "path");
        if (!key) { send_rsp_err(srv, id, "missing key"); return; }
        // map UI key -> project.json field name
        std::string field;
        if      (*key == "include")   field = "include_dir";
        else if (*key == "opencv")    field = "opencv_dir";
        else if (*key == "turbojpeg") field = "turbojpeg_root";
        else if (*key == "ipp")       field = "ipp_root";
        else if (*key == "vcvars")    field = "vcvars";
        else { send_rsp_err(srv, id, "unknown toolchain key: " + *key); return; }
        std::string err;
        if (!write_toolchain_override_(g_project_folder, field, path ? *path : std::string(), err)) {
            send_rsp_err(srv, id, "set_toolchain_override failed: " + err);
            return;
        }
        // Re-resolve globals so the next compile reflects the change; the editor's
        // IntelliSense config is the VS Code extension's job (it re-reads the
        // health below). The core no longer writes .vscode.
        resolve_toolchain_(g_project_folder);
        std::string data = "{\"applied\":true,\"recompile_needed\":true,\"health\":";
        data += toolchain_health_json_(g_project_folder);
        data += "}";
        send_rsp_ok(srv, id, data);
    } else {
        send_rsp_err(srv, id, std::string("unknown command: ") + name);
    }
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
                g_include_dir = (p / "include").string();
                break;
            }
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
        if (g_include_dir.empty()) {
            // Fallback: next to the exe.
            g_include_dir = (std::filesystem::path(xi::cli::get_exe_dir()) / "include").string();
        }
        // Remember the shipped headers as the default a project override falls
        // back to (see resolve_toolchain_).
        g_include_dir_default = g_include_dir;
    }
    g_work_dir = (std::filesystem::temp_directory_path() / "xinsp2").string();
    std::filesystem::create_directories(g_work_dir);

    // Probe accelerators once. Logged so the user can see what their
    // compiled scripts will inherit.
    g_opencv_dir     = xi::script::detail::probe_opencv_dir();
    g_turbojpeg_root = xi::script::detail::probe_turbojpeg_root();
    g_ipp_root       = xi::script::detail::probe_ipp_root();
    std::fprintf(stderr, "[xinsp2] script-side accelerators: opencv=%s  turbojpeg=%s  ipp=%s\n",
                 g_opencv_dir.empty()     ? "no" : g_opencv_dir.c_str(),
                 g_turbojpeg_root.empty() ? "no" : g_turbojpeg_root.c_str(),
                 g_ipp_root.empty()       ? "no" : g_ipp_root.c_str());

    // Find and scan plugins directory (sibling of backend/)
    {
        std::filesystem::path p = xi::cli::get_exe_dir();
        for (int i = 0; i < 6; ++i) {
            if (std::filesystem::exists(p / "plugins")) {
                g_plugins_dir = (p / "plugins").string();
                break;
            }
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
    }
    // G1.3 — certify each discovered plugin in a throwaway child (this same
    // backend exe, --certify-plugin mode) before arming it during the scan. A DLL
    // that crashes certification is skipped + surfaced (g_plugin_mgr.certify_
    // warnings()), so discovery can never load a known-bad DLL into the backend.
    {
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (n) g_plugin_mgr.set_certify_exe(std::string(exe, n));
    }
    if (!g_plugins_dir.empty()) {
        int n = g_plugin_mgr.scan_plugins(g_plugins_dir);
        std::fprintf(stderr, "[xinsp2] scanned %d plugins from %s\n", n, g_plugins_dir.c_str());
    }
    // Additional plugin folders from --plugins-dir / XINSP2_EXTRA_PLUGIN_DIRS.
    // Lets external SDKs keep their plugin DLLs in place — no copy needed.
    for (auto& dir : xi::cli::parse_extra_plugin_dirs(argc, argv)) {
        if (!std::filesystem::exists(dir)) {
            std::fprintf(stderr, "[xinsp2] extra plugin dir not found: %s\n", dir.c_str());
            continue;
        }
        int n = g_plugin_mgr.scan_plugins(dir);
        std::fprintf(stderr, "[xinsp2] scanned %d plugins from %s\n", n, dir.c_str());
    }

    // G1.3 / G2.2 — surface any plugin gated out at discovery (certify crashed, or
    // FE-quarantined) into the recent-errors ring so cmd:recent_errors + the
    // extension toast tell the operator WHICH plugin is disabled and why (not just
    // a silently-missing plugin). The scan already logged each to stderr.
    for (auto& w : g_plugin_mgr.certify_warnings())
        push_recent_error("plugin", w.reason);

    std::fprintf(stderr, "[xinsp2] include_dir=%s\n", g_include_dir.c_str());
    std::fprintf(stderr, "[xinsp2] work_dir=%s\n",    g_work_dir.c_str());
    std::fprintf(stderr, "[xinsp2] plugins_dir=%s\n",  g_plugins_dir.c_str());

    // Process isolation + SHM removed 2026-05: all plugins run
    // in-process and share the host ImagePool directly (zero-copy via
    // pointers, no cross-process marshalling). No worker process, no
    // shared-memory region to set up.

    // Hand the same compile environment that xi::script::compile uses
    // to the plugin manager — project plugins (compiled when a project
    // is opened) need the include dir, vcvars, and accelerator roots.
    xi::CompileEnv env;
    env.include_dir    = g_include_dir;
    env.opencv_dir     = g_opencv_dir;
    env.turbojpeg_root = g_turbojpeg_root;
    env.ipp_root       = g_ipp_root;
    // --aot: this is a prebuilt bundle — load existing plugin DLLs instead of
    // compiling (no cl.exe on the target). The autostart script should point at a
    // .dll too (compile_and_load loads a .dll directly).
    env.aot = xi::cli::has_flag(argc, argv, "--aot");
    if (env.aot) std::fprintf(stderr, "[xinsp2] AOT mode: loading prebuilt plugin/script DLLs (no compiler)\n");
    g_plugin_mgr.set_compile_env(env);

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
            std::lock_guard<std::mutex> lk(g_recent_errors_mu);
            g_recent_errors.clear();
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

    g_watchdog_ms = xi::cli::parse_watchdog_ms(argc, argv);
    if (g_watchdog_ms.load() > 0) {
        std::fprintf(stderr, "[xinsp2] watchdog enabled: %d ms per inspect\n", g_watchdog_ms.load());
    }
    g_srv_for_bp = &srv;   // status_cb + dropped-frame markers emit through it
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
        if (auto* s = g_srv_for_bp.load(std::memory_order_acquire))
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
        if (auto* s = g_srv_for_bp.load(std::memory_order_acquire)) {
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
    g_watchdog_run = true;
    g_watchdog_thread = std::thread([&srv]() {
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
        while (g_watchdog_run.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            int64_t now = now_ms();
            bool any_overran = false;
            for (int i = 0; i < WD_SLOTS; ++i) {
                int64_t dl = g_wd_deadlines[i].load();
                if (dl != 0 && now >= dl) { wd_snap[i] = dl; any_overran = true; }
                else                       { wd_snap[i] = 0; }
            }
            if (!any_overran) continue;

            // Phase 1: cooperative cancel + grace. Log the attempt so the
            // escalation is observable (and a hard trip can be proven to have
            // tried the soft cancel first, not jumped straight to the kill).
            std::fprintf(stderr,
                "[xinsp2] watchdog: inspect overran %dms — requesting cooperative cancel\n",
                g_watchdog_ms.load());
            {
                std::lock_guard<std::mutex> lk(g_script_mu);
                if (g_script.set_global_cancel) g_script.set_global_cancel(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            // Did every inspect we TARGETED return? (Its slot is now free or
            // re-armed by a different inspect with a different deadline.) Match
            // on slot index AND deadline value so a fresh inspect reusing the
            // slot is not mistaken for the original stuck one.
            bool still_stuck = false;
            for (int i = 0; i < WD_SLOTS; ++i) {
                if (wd_snap[i] != 0 && g_wd_deadlines[i].load() == wd_snap[i]) {
                    still_stuck = true; break;
                }
            }
            if (!still_stuck) {
                {
                    std::lock_guard<std::mutex> lk(g_script_mu);
                    if (g_script.set_global_cancel) g_script.set_global_cancel(0);
                }
                int n = ++g_watchdog_trips;
                std::fprintf(stderr,
                    "[xinsp2] watchdog tripped (#%d) — script honoured cooperative cancel\n", n);
                emit_error_log(srv,
                    "watchdog tripped — inspect exceeded "
                    + std::to_string(g_watchdog_ms.load())
                    + "ms; cooperative cancel succeeded");
                continue;
            }

            // Phase 2: hard trip — exit for FE respawn (see header above).
            ++g_watchdog_trips;
            std::fprintf(stderr,
                "[xinsp2] watchdog HARD trip - inspect exceeded %dms and ignored "
                "cooperative cancel; exiting for supervisor respawn (rc=0x%04X)\n",
                g_watchdog_ms.load(), WATCHDOG_EXIT_CODE);
            emit_error_log(srv,
                "watchdog HARD trip — inspect exceeded "
                + std::to_string(g_watchdog_ms.load())
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
    // file-scope g_watchdog_thread) invokes std::terminate, which the still-armed
    // crash filter turns into a spurious minidump + abnormal exit — the FE reads a
    // routine port-in-use as a backend CRASH. The normal epilogue joins too; this
    // then no-ops (joinable()==false).
    struct WatchdogJoiner {
        ~WatchdogJoiner() {
            g_watchdog_run = false;
            if (g_watchdog_thread.joinable()) g_watchdog_thread.join();
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
                script = g_plugin_mgr.project().script_path;
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
                if (!g_script.ok()) {
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
            while (!g_should_exit.load()) {
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
        while (!g_should_exit.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
    while (!g_should_exit.load() && srv.is_running()) {
        srv.poll(100);
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - hb_last_ms >= 1000) { write_heartbeat(); hb_last_ms = now; }
    }

    g_watchdog_run = false;
    if (g_watchdog_thread.joinable()) g_watchdog_thread.join();   // join before teardown nulls srv
    // Controlled teardown before `srv` (a main() local captured by the bus sink +
    // g_srv_for_bp) leaves scope, and while the ImagePool/TriggerBus singletons
    // are still alive — covers exits that didn't go through cmd:shutdown (e.g.
    // g_should_exit flipped elsewhere). Single source of truth; idempotent with
    // the shutdown handler. Runs BEFORE srv.stop() so the pool's workers are
    // joined (no emit) before the server goes away.
    controlled_shutdown_teardown_();
    srv.stop();
    std::fprintf(stderr, "[xinsp2] shutdown complete\n");
    return 0;
}
