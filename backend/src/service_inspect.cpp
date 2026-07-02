//
// service_inspect.cpp — the single-frame inspection cycle: COMPUTE half (script
// invocation + SEH boundary + crash breadcrumb + watchdog) and EMISSION half
// (metrics + ordered-emit gate + staged flush + result/events), plus the thin
// run_one_inspection driver. Split from service_main.cpp (behavior-preserving;
// see service_internal.hpp).
//
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <yyjson.h>
#include <xi/xi_metrics.hpp>

#include "service_internal.hpp"

using xi::EmitGate;
using xi::EmitTurn;

// Run one full inspection cycle: reset → inspect → emit.
// The inspect call is wrapped in SEH so a script crash (null deref,
// divide-by-zero, stack overflow) is caught without killing the backend.
// F-P1-1 run lifecycle event (run_started / run_finished / run_error). Immediate
// (NOT gated) — a diagnostic bracket around the inspect. Lifted out of the old inline
// lambda so both halves of the split (compute emits run_started, emission emits
// run_finished/run_error) produce byte-identical wire shape. Documented in
// docs/reference/ws-protocol.md.
static void emit_run_event_(xi::ws::Server& srv, int64_t run_id, const char* name,
                            const std::string& extra_data = "") {
    xp::Event ev;
    ev.name = name;
    std::string data = "{\"run_id\":" + std::to_string(run_id);
    if (!extra_data.empty()) { data += ","; data += extra_data; }
    data += "}";
    ev.data_json = data;
    srv.send_text(ev.to_json());
}

// The run context carried across the compute→emit seam (design review lens 2#5). The
// COMPUTE half fills it; the EMISSION half reads it — so emission never re-reads the
// globals/thread-locals compute snapshotted (rr_source/rr_group off the thread_local
// trigger, the script handle). Deliberately NOT here: the EmitTurn and StagedEmitGuard,
// whose RAII lifetimes straddle the seam and MUST stay in the driver.
struct RunOutcome {
    xi::script::LoadedScript s;      // script snapshot (also used for the set_run_context clear)
    bool        inspect_ok = false;  // set exactly once by compute
    std::string run_error_what;      // run_error payload (empty on success)
    int64_t     dt_us = 0;           // inspect latency, measured once after inspect
    std::string rr_source, rr_group; // Result provenance snapshot (this thread's trigger)
    std::string rr_trigger_hex;      // this run's 128-bit trigger id as hex (additive; empty if none)
    int64_t     rr_script_gen = 0;   // active-script generation snapshotted at run start (0 = unknown)
};

// ---- COMPUTE half: script invocation + SEH boundary + crash breadcrumb + watchdog --
// Snapshots the script under g_eng.script_mu, arms the watchdog slot, stamps this thread's
// crash breadcrumb, then runs reset()+inspect() inside the SEH/std::exception/... catch
// boundary, recording ok/error + latency into `out`. The inspect_tv-vs-inspect selection
// stays exactly as-is (invariant 5). The watchdog is disarmed on EVERY exit path — the
// success tail and all three catches (invariant 3) — because the whole try/catch lives
// here intact. Immediate diagnostics (emit_error_log → be_log/recent-errors, the
// run_started event) fire here just as on the old inline path. Nothing here touches the
// emit gate or metrics — that is the emission half's job. Returns false ONLY when no
// script is loaded, so the driver can early-return (letting its turn + staged guard
// dtors run, unchanged).
static bool run_inspection_compute_(xi::ws::Server& srv, int frame_hint,
                                    int64_t run_id, const std::string& frame_path,
                                    RunOutcome& out) {
    {
        std::lock_guard<std::mutex> lk(g_eng.script_mu);
        out.s = g_eng.script;
        // Snapshot the active-script generation under the SAME lock as the
        // script handle, so the reported generation is exactly the one that
        // owns the DLL this run will call. A swap to N+1 that happens mid-run
        // can't change this run's reported N (we captured it here, not at
        // emit). 0 stays 0 when no script has ever loaded.
        out.rr_script_gen = g_eng.script_generation.load(std::memory_order_relaxed);
    }
    xi::script::LoadedScript& s = out.s;

    if (!s.ok()) {
        xp::LogMsg lm;
        lm.level = "warn";
        lm.msg = "no script loaded — compile a .cpp first";
        srv.send_text(lm.to_json());
        return false;
    }

    // Plumb the optional per-run context (frame_path) into the script
    // DLL's globals before inspect runs. Cleared on the way out (in the
    // emission half, success path) so a subsequent run with no frame_path
    // arg sees an empty string, not the previous value.
    if (s.set_run_context) s.set_run_context(frame_path.c_str());

    // Per-run Result: reset to NA before the script runs, and snapshot the
    // source/group provenance from this thread's trigger (thread_local, valid
    // for the duration of the inspect). The script sets the result via
    // xi::result() → result_cb → g_run_result; emission reads it below in the gate.
    g_run_result = RunResult{};
    if (g_current_trigger) {
        out.rr_source = g_current_trigger->leader_source;
        out.rr_group  = g_current_trigger->group;
        out.rr_trigger_hex = trigger_id_hex(g_current_trigger->id);   // additive: this run's trigger id
    }

    emit_run_event_(srv, run_id, "run_started");

    auto t0 = std::chrono::steady_clock::now();
    // Arm the watchdog: claim a per-inspect slot holding this inspect's
    // deadline. Works for any dispatch_threads (N slots), unlike the old
    // single-slot scheme that had to skip N>1. Cleared below regardless of
    // throw. No thread handle is kept — a hard trip exits the process (FE
    // respawns) rather than TerminateThread'ing a worker (which would leak the
    // per-instance lock + risk heap corruption).
    int wd_slot = -1;
    int wd_ms = g_eng.watchdog_ms.load();
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
    // emitting yet — emission happens in emit_run_outcome_ under the EmitTurn gate
    // so ordered mode (parallelism.result_order: "arrival") can serialize the wire
    // stream by frame-arrival order. Logs (diagnostic) stay immediate; the run_error
    // EVENT is deferred so it's ordered with run_finished and the turn always
    // advances (an error must not stall the ordered stream).
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
        out.inspect_ok = true;
    } catch (const seh_exception& e) {
        disarm();
        auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0).count();
        char msg[256];
        std::snprintf(msg, sizeof(msg), "script crashed after %lldms: 0x%08X (%s)",
                     (long long)dt_ms, e.code, e.what());
        std::fprintf(stderr, "[xinsp2] %s\n", msg);
        emit_error_log(srv, msg, run_id);
        out.run_error_what = "\"what\":";
        xp::json_escape_into(out.run_error_what, std::string(msg));
    } catch (const std::exception& e) {
        disarm();
        std::fprintf(stderr, "[xinsp2] inspect threw: %s\n", e.what());
        emit_error_log(srv, std::string("script exception: ") + e.what(), run_id);
        out.run_error_what = "\"what\":";
        xp::json_escape_into(out.run_error_what, std::string("script exception: ") + e.what());
    } catch (...) {
        disarm();
        // Align with the named catches above: also leave a stderr breadcrumb +
        // push to the recent-errors ring, else an unknown (non-std) throw vanished
        // from both be_log and cmd:recent_errors.
        std::fprintf(stderr, "[xinsp2] inspect threw a non-std exception (run_id=%lld)\n", (long long)run_id);
        emit_error_log(srv, "script threw a non-std exception", run_id);
        out.run_error_what = "\"what\":\"unknown_exception\"";
    }

    out.dt_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();
    return true;
}

// ---- EMISSION half: metrics + ordered-emit gate + staged flush + result/events -----
// Runs strictly AFTER compute. `turn` is the driver's EmitTurn — it was CLAIMED before
// compute (invariant 1); here we only wait_turn()/complete() it, its RAII lifetime + the
// dtor backstop stay in the driver. Records the frame metric exactly once, on every path
// (invariant 4), then, inside the gate, flushes staged sinks in frame order on success
// and emits the one Result + run_finished/run_error. On a STOP wake (my_turn == false)
// the staged flush is SKIPPED so the driver's StagedEmitGuard drops them (invariant 2,
// matching the crash path).
static void emit_run_outcome_(xi::ws::Server& srv, int64_t run_id,
                              EmitTurn& turn, RunOutcome& out) {
    auto dt_ms = out.dt_us / 1000;   // wire/event latency stays integer-ms (unchanged)
    // OQ-7a observability: record this frame's latency + ok/error into the process
    // metrics registry (lock-free). Sub-ms precision from dt_us so fast frames don't
    // all collapse into the 0-bucket. Exported via cmd:metrics (see dispatch below).
    xi::MetricsRegistry::instance().record_frame((double)out.dt_us / 1000.0, out.inspect_ok);
    // Ordered mode: block until it's this frame's turn to emit, then advance the
    // cursor (turn.complete()) right after so the next worker can emit promptly.
    // The turn was claimed BEFORE compute; its dtor (in the driver) is the backstop
    // if anything throws. No-op for emit_seq < 0 (completion mode).
    bool my_turn = turn.wait_turn();
    if (out.inspect_ok) {
        // Deliver this frame's staged sink calls (comm/expose/…) IN FRAME ORDER —
        // inside the gate, before the wire result, so a sink's side effect is
        // serialized with the run's output. A failed inspect skips this; the
        // guard then drops the partial sends (don't push a crashed frame to PLC).
        // On a STOP wake (my_turn false: the lane stopped before this seq's turn,
        // so every parked seq woke at once) also skip — flushing here would deliver
        // out of frame order and concurrently to the same sink. The staged guard
        // drops them, matching the crash path ("don't push this frame to the PLC").
        if (my_turn) flush_staged_emits_(run_id);
        // One Result per run, emitted before run_finished so consumers can pair
        // them. Ordered with the rest of the stream by the gate. BREAKING: a run
        // that completed but set no xi::result now emits XI_SYS_NO_VERDICT
        // (class="no_verdict"), where v1 emitted 0/NA. A script that explicitly
        // set a verdict — including a legitimate NA (0) — is passed through as-is.
        int rr_code = g_run_result.set ? g_run_result.code : XI_SYS_NO_VERDICT;
        std::string rr_msg = g_run_result.set ? g_run_result.msg
                                              : std::string("no verdict (script set no result)");
        emit_run_result(srv, rr_code, rr_msg,
                        run_id, dt_ms, out.rr_source, out.rr_group,
                        out.rr_trigger_hex, nullptr, nullptr,
                        out.rr_script_gen);   // class derived from code (ok/ng/na/no_verdict)
        // run_finished carries the run's INSPECT COMPUTE time. `ms` is the legacy
        // integer-ms field (kept, unchanged wire value) — it is inspect compute
        // ONLY (excludes queue wait, emit-gate wait, staged sink flush, JPEG encode,
        // WS send), NOT cycle/decision latency. The additive `inspect_compute_us`
        // field states that meaning explicitly at µs precision (external review 05
        // #7). BREAKING (staged, not on master): consumers should migrate to
        // `inspect_compute_us`; `ms` is retained for back-compat.
        emit_run_event_(srv, run_id, "run_finished",
                        "\"ms\":" + std::to_string((long long)dt_ms) +
                        ",\"inspect_compute_us\":" + std::to_string((long long)out.dt_us));
        // Clear so the next run, if it doesn't carry a frame_path arg,
        // sees an empty path instead of the stale previous one.
        if (out.s.set_run_context) out.s.set_run_context("");
    } else {
        // Inspect failed (crash/throw) — still emit one Result so the stream
        // has no gap. BREAKING: the numeric code is now XI_SYS_CRASHED (was 0/NA),
        // so a caught inspect error is no longer indistinguishable from a legit NA
        // verdict on the numeric channel. class="crashed" derives from the code
        // (passed explicitly too), reason_code carries the specific cause.
        emit_run_result(srv, XI_SYS_CRASHED, "inspect error", run_id, dt_ms,
                        out.rr_source, out.rr_group,
                        out.rr_trigger_hex, "crashed", "inspect_error",
                        out.rr_script_gen);
        emit_run_event_(srv, run_id, "run_error", out.run_error_what);
    }
    turn.complete();   // advance the gate now (dtor would otherwise do it at fn exit)
}

// Thin driver: owns the two RAII guards whose lifetimes straddle the compute→emit seam,
// then calls compute → emission. Deliberately holds no inspect logic of its own.
void run_one_inspection(xi::ws::Server& srv, int frame_hint,
                               int64_t run_id, const std::string& frame_path,
                               int64_t emit_seq, EmitGate* gate) {
    if (run_id == 0) run_id = ++g_eng.run_id;

    // INVARIANT 1: claim the ordered-emit turn NOW — BEFORE compute — so claim order ==
    // arrival order (the gate replays emission in claim order). Inert until wait_turn()
    // in the emission half, so compute stays parallel. Its dtor releases the turn on
    // EVERY exit below (early-return / a future throw) so an orphaned seq can't deadlock
    // the lane. No-op for emit_seq < 0. Kept in the driver: its lifetime spans compute
    // AND emit, so it must not be split across the two halves.
    EmitTurn turn(gate, emit_seq, &g_eng.continuous);

    // INVARIANT 2: drains any sink calls this inspect staged but didn't flush (no script,
    // crash, STOP-wake skip). The success path flushes (empties g_staged) so this no-ops.
    // Its scope is the WHOLE driver so it drains on every exit path — again a lifetime
    // that straddles the seam, so it stays in the driver, never inside a half.
    StagedEmitGuard staged_guard;

    RunOutcome out;
    if (!run_inspection_compute_(srv, frame_hint, run_id, frame_path, out))
        return;   // no script loaded — turn + staged_guard dtors run (unchanged early-out)

    emit_run_outcome_(srv, run_id, turn, out);
}
