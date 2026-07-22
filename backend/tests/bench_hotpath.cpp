//
// bench_hotpath.cpp — the ACTUAL per-frame inspection hot path, end to end.
//
// WHY THIS BENCH EXISTS (05 review, finding #6 + the P1 "measure the real hot
// path" batch):
//   bench_record measures ONE data-layer op (a Record crossing a plugin
//   boundary). bench_image_pool measures ONE op (pool create/release). Neither
//   measures the thing production actually pays PER FRAME: a source emit funneled
//   through the trigger bus, handed to a dispatch lane, popped by a worker,
//   inspected, and emitted back through the ordered-result gate. This bench times
//   THAT span — emit → ordered result — exercising the REAL runtime primitives:
//     - xi::TriggerBus      (the emit_pack dispatch funnel, PACK-only post-CUT)
//     - xi::ImagePool       (pooled frames, addref/release refcount, no copy)
//     - xi::PackRegistry / xi.pack@1 (the sealed keyed-buffer pack the frame rides)
//     - xi::EmitGate/EmitTurn (arrival-ordered result emission)
//     - xi::MetricsRegistry (the same record_frame the live path calls)
//
// POST-CUT (v11→v12 ABI break): the frame now rides an xi.pack@1 sealed pack —
// the producer seals one pooled image ("frame") into a pack and emits it via the
// real TriggerBus::emit_pack; the worker reads the image back through the pack
// accessor. The Record/DocRegistry routing-meta doc (and the --no-meta knob that
// toggled it) was RETIRED at THE CUT: TriggerEvent no longer carries images or a
// meta_doc, only a pack handle. Everything else about the span is unchanged.
//
// WHAT IS REAL vs STUBBED (truth-in-labeling — this bench links xi_core, which is
// header-only, so the service_*.cpp translation units are NOT available to it):
//   REAL:    TriggerBus, ImagePool, PackRegistry (xi.pack@1), EmitGate/EmitTurn,
//            MetricsRegistry — every allocation / refcount / cross-thread-handoff /
//            ordering primitive on the per-frame path.
//   STUBBED: the dispatch lane's queue plumbing (GroupLane, welded into the
//            backend exe behind the private service_internal.hpp) is reproduced
//            here as MiniLane — a faithful copy of GroupLane's shape and its
//            worker loop (service_dispatch.cpp spawn_group_pool_): same deque +
//            mutex + cv, same enqueue drop policy (enqueue_to_lane_), same
//            dequeue → arrival_id → eseq → dequeued_at_us stamp → EmitTurn emit →
//            release sequence. The queue plumbing is not itself a per-frame cost
//            center (see bench_image_pool's finding); what MiniLane preserves is
//            the cross-thread handoff and ordering that DO cost.
//   NOT MODELLED (out of this bench's span, on purpose — the review lists these as
//            separate stages to measure elsewhere): the script/plugin compute
//            (replaced by a fixed tiny touch so the number reflects PIPELINE
//            overhead, not vision work), JPEG/expose observation sinks, WebSocket
//            send, and PLC/MES delivery. The bench's name and metric keys say
//            "emit_to_result" so nobody reads it as full decision+delivery latency.
//
// SPAN MEASURED: t_emit (just before TriggerBus::emit, steady clock) → the moment
//   this frame's result clears the ordered EmitTurn. That is: source-emit +
//   bus funnel + queue wait + dispatch handoff + inspect compute + ordered-gate
//   wait + result emit. It EXCLUDES frame capture/allocation (the frame already
//   exists when a real source emits it) and downstream sink/transport/delivery.
//
// TWO LOAD MODELS:
//   closed-loop (--inflight K): at most K frames outstanding — the producer waits
//     for a completion before offering the next. Measures STEADY-STATE service
//     latency under matched load (little/no unbounded queue buildup). This is the
//     stable, gate-able signal.
//   open-loop  (--pace-us U / burst): offer N frames at a fixed spacing (0 =
//     burst). Shows the real TAIL — queue wait grows, head-of-line blocking under
//     arrival ordering — which best-of gates must NOT pretend to measure (05 #2).
//
// OUTPUT: p50/p95/p99/max/mean latency + throughput + drops per scenario (05 #2:
//   distributions, not just means). --gate emits the machine-readable GATE lines
//   (a best-of MEDIAN of the closed-loop steady-state latency — a micro-throughput
//   regression gate, NOT a p99 claim) + the env fingerprint (05 #3), wired exactly
//   like perf_image_pool / perf_record (SKIP-with-reason off the capture machine).
//
#include "perf_fingerprint.hpp"

#include "xi/xi_abi.h"
#include "xi/xi_clock.hpp"
#include "xi/xi_emit_gate.hpp"
#include "xi/xi_image_pool.hpp"
#include "xi/xi_metrics.hpp"
#include "xi/xi_pack_abi.hpp"
#include "xi/xi_trigger_bus.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;

// Stop the optimizer eliding the (tiny) fixed inspect touch.
static volatile uint64_t g_sink = 0;

// ---------------------------------------------------------------------------
// MiniLane — a faithful stand-in for GroupLane (service_internal.hpp:126) and
// its worker loop (service_dispatch.cpp spawn_group_pool_). Same fields that
// matter to the per-frame path: the event deque, its mutex+cv, the ordered
// EmitGate + seq_next cursor, the queue_depth/overflow drop policy, and the
// arrival_id counter (== g_eng.run_id, claimed at ENQUEUE in FIFO order).
// ---------------------------------------------------------------------------
struct MiniLane {
    std::deque<xi::TriggerEvent> q;
    std::mutex                   mu;
    std::condition_variable      cv;
    std::vector<std::thread>     workers;
    xi::EmitGate                 gate;
    bool                         ordered = false;
    std::atomic<int64_t>         seq_next{0};
    std::atomic<int64_t>         run_id{0};       // mirrors g_eng.run_id
    int                          queue_depth = 1024;
    std::string                  overflow = "drop_oldest";
    std::atomic<uint64_t>        dropped{0};
    std::atomic<uint64_t>        high_watermark{0};
};

struct Config {
    int  width = 320, height = 240, channels = 3;   // a typical CV frame (bench_image_pool)
    int  parallel = 1;                              // worker threads (max_parallel)
    int  frames = 20000;                            // total frames offered
    bool ordered = false;                           // result_order == "arrival"
    int  inflight = 0;                              // >0 => closed-loop, this many outstanding
    int  pace_us = 0;                               // open-loop inter-emit spacing (0 = burst)
    int  work = 256;                                // fixed inspect touch: sample this many pixels
    int  queue_depth = 1024;
};

struct Result {
    std::vector<int64_t> lat_us;   // per-completed-frame emit→ordered-result latency
    uint64_t drops = 0;
    double   elapsed_s = 0;        // first emit → last result
    uint64_t completed = 0;
};

// The fixed tiny "inspect": touch `work` strided pixels of the pooled frame and
// fold them into a sink. Fixed iteration count (independent of frame size) so the
// measured latency reflects PIPELINE overhead, not vision compute — the review's
// "minimal inspect entry (no-op or fixed tiny compute)".
static inline void tiny_inspect(const uint8_t* px, size_t nbytes, int work) {
    if (!px || nbytes == 0) return;
    uint64_t acc = 0;
    size_t step = nbytes > (size_t)work ? nbytes / (size_t)work : 1;
    for (size_t i = 0; i < nbytes; i += step) acc += px[i];
    g_sink += acc;
}

// Release a consumed (or dropped) event exactly like release_trigger_event_
// (service_sinks.cpp) does post-CUT: the pack IS the payload, so a single pack
// release drops the frame (and its pooled image) the event carried. No-op on a
// XI_PACK_NULL event.
static void release_event_(xi::TriggerEvent& ev) {
    xi::TriggerBus::instance().release_pack_(ev.pack);
    ev.pack = XI_PACK_NULL;
}

// One full run. Spins up `cfg.parallel` workers on a MiniLane, installs the
// TriggerBus sink that enqueues into it (mirroring install_trigger_sink_ /
// enqueue_to_lane_), offers `cfg.frames` frames under the chosen load model, and
// collects per-frame end-to-end latency.
static Result run_scenario(const Config& cfg) {
    auto& bus   = xi::TriggerBus::instance();
    auto& pool  = xi::ImagePool::instance();
    const xi_pack_v1* pk = xi::pack_v1_iface();   // installed once in main()

    MiniLane lane;
    lane.ordered     = cfg.ordered && cfg.parallel > 1;   // ordered only with real concurrency
    lane.queue_depth = cfg.queue_depth;

    std::atomic<bool>    keep_going{true};
    std::atomic<uint64_t> completed{0};

    // Per-frame latency slots, indexed by arrival_id-1 (unique, FIFO order).
    std::vector<int64_t> lat((size_t)cfg.frames, -1);

    // Closed-loop permit: at most `inflight` frames outstanding. mutex+cv counting
    // semaphore (portable). Unused (huge budget) in open-loop.
    std::mutex           permit_mu;
    std::condition_variable permit_cv;
    int                  permits = cfg.inflight > 0 ? cfg.inflight : cfg.frames + 1;

    auto release_permit = [&] {
        { std::lock_guard<std::mutex> lk(permit_mu); ++permits; }
        permit_cv.notify_one();
    };

    // ---- workers: the real dequeue→inspect→ordered-emit→release loop ----------
    for (int w = 0; w < cfg.parallel; ++w) {
        lane.workers.emplace_back([&] {
            while (keep_going.load()) {
                xi::TriggerEvent ev; bool have = false; int64_t rid = 0; int64_t eseq = -1;
                {
                    std::unique_lock<std::mutex> lk(lane.mu);
                    lane.cv.wait(lk, [&] { return !lane.q.empty() || !keep_going.load(); });
                    if (!keep_going.load() && lane.q.empty()) break;
                    if (!lane.q.empty()) {
                        ev = std::move(lane.q.front()); lane.q.pop_front(); have = true;
                        rid  = ev.arrival_id;
                        if (lane.ordered) eseq = lane.seq_next.fetch_add(1);
                    }
                }
                lane.cv.notify_one();
                if (!have) continue;

                ev.dequeued_at_us = xi::wall_us();   // same stamp the real worker makes

                // --- read the frame back off the pack (the real consume path), then
                //     the fixed tiny inspect, timed exactly like run_inspection_compute_
                xi_pack_image iv{};
                pk->get_image(ev.pack, "frame", &iv);
                auto t0 = clk::now();
                tiny_inspect((const uint8_t*)iv.pixels, (size_t)iv.length, cfg.work);
                double dt_us = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
                xi::MetricsRegistry::instance().record_frame(dt_us / 1000.0, /*ok=*/true);

                // --- ordered result emit: the real EmitTurn gate (no-op when eseq<0)
                xi::EmitTurn turn(&lane.gate, eseq, &keep_going);
                turn.wait_turn();
                // "emit the result": stamp end-to-end latency for this frame.
                int64_t now = xi::mono_us();
                lat[(size_t)rid - 1] = now - ev.timestamp_us;   // timestamp_us == steady t_emit
                turn.complete();

                release_event_(ev);
                completed.fetch_add(1, std::memory_order_relaxed);
                if (cfg.inflight > 0) release_permit();
            }
        });
    }

    // ---- sink: TriggerBus → MiniLane enqueue (mirrors enqueue_to_lane_) --------
    bus.set_sink([&](xi::TriggerEvent ev) {
        std::unique_lock<std::mutex> lk(lane.mu);
        if ((int)lane.q.size() < lane.queue_depth) {
            ev.arrival_id = lane.run_id.fetch_add(1) + 1;   // FIFO arrival id, 1-based
            lane.q.push_back(std::move(ev));
            uint64_t ns = lane.q.size(), prev = lane.high_watermark.load();
            while (ns > prev && !lane.high_watermark.compare_exchange_weak(prev, ns)) {}
            lane.cv.notify_one();
            return;
        }
        // Queue full: drop_oldest (default) / drop_newest — release the dropped
        // event's refs so a full queue never leaks a pool slot. A dropped frame
        // still consumes an arrival id (kept/dropped share one sequence).
        lane.dropped.fetch_add(1, std::memory_order_relaxed);
        if (lane.overflow == "drop_newest") {
            lk.unlock();
            release_event_(ev);
            if (cfg.inflight > 0) release_permit();   // its permit is freed by the drop
        } else {
            xi::TriggerEvent old = std::move(lane.q.front()); lane.q.pop_front();
            ev.arrival_id = lane.run_id.fetch_add(1) + 1;
            lane.q.push_back(std::move(ev));
            lane.cv.notify_one();
            lk.unlock();
            // The evicted frame will never complete: reclaim its slot + permit.
            release_event_(old);
            if (cfg.inflight > 0) release_permit();
        }
    });

    // ---- producer: offer cfg.frames frames under the load model ---------------
    auto t_start = clk::now();
    for (int i = 0; i < cfg.frames; ++i) {
        if (cfg.inflight > 0) {
            std::unique_lock<std::mutex> lk(permit_mu);
            permit_cv.wait(lk, [&] { return permits > 0; });
            --permits;
        } else if (cfg.pace_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(cfg.pace_us));
        }
        // A real source hands the bus an ALREADY-captured pooled frame, sealed into
        // an xi.pack@1 pack. create() the pooled frame; builder_add_image COPIES its
        // pixels into the pack's own pooled image (the realistic capture→pack hop),
        // so we drop our create ref right after. seal → emit_pack (the host forwarder
        // takes its OWN event ref) → release our seal ref, exactly the door pattern
        // test_pack_door §4 uses. The pack (and its pooled image) then rides the
        // event; the worker frees it via release_pack_.
        xi_image_handle h = pool.create(cfg.width, cfg.height, cfg.channels);
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_image(b, "frame", cfg.width, cfg.height, cfg.channels, pool.data(h));
        xi_pack_handle f = pk->builder_seal(b);   // seal ref (rc 1)
        pool.release(h);                          // pack copied the pixels; drop create ref
        int64_t t_emit = xi::mono_us();
        pk->emit_pack("bench_cam", xi::make_trigger_id(), f, t_emit);  // event takes a 2nd ref
        pk->release(f);   // drop our seal ref; the event holds the dispatch ref
    }

    // ---- drain: wait until every non-dropped frame has completed --------------
    uint64_t expect = (uint64_t)cfg.frames;   // drops decrement the target
    for (;;) {
        uint64_t done = completed.load(), dr = lane.dropped.load();
        if (done + dr >= expect) break;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    double elapsed = std::chrono::duration<double>(clk::now() - t_start).count();

    // ---- teardown -------------------------------------------------------------
    keep_going.store(false);
    { std::lock_guard<std::mutex> lk(lane.mu); }
    lane.cv.notify_all();
    { std::lock_guard<std::mutex> lk(lane.gate.mu); }
    lane.gate.cv.notify_all();
    for (auto& t : lane.workers) t.join();
    bus.clear_sink();

    Result r;
    r.drops     = lane.dropped.load();
    r.completed = completed.load();
    r.elapsed_s = elapsed;
    r.lat_us.reserve(lat.size());
    for (int64_t v : lat) if (v >= 0) r.lat_us.push_back(v);
    return r;
}

static int64_t pct(std::vector<int64_t>& v, double p) {
    if (v.empty()) return 0;
    size_t idx = (size_t)(p / 100.0 * (v.size() - 1) + 0.5);
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];   // v must be sorted
}

static void print_dist(const char* label, Result& r) {
    std::sort(r.lat_us.begin(), r.lat_us.end());
    double mean = 0;
    for (int64_t v : r.lat_us) mean += (double)v;
    if (!r.lat_us.empty()) mean /= (double)r.lat_us.size();
    double thru = r.elapsed_s > 0 ? (double)r.completed / r.elapsed_s : 0.0;
    std::printf("  %-28s  n=%-6zu  p50=%6lld  p95=%7lld  p99=%7lld  max=%8lld  mean=%8.1f us   "
                "%9.0f frames/s   drops=%llu\n",
                label, r.lat_us.size(),
                (long long)pct(r.lat_us, 50), (long long)pct(r.lat_us, 95),
                (long long)pct(r.lat_us, 99), (long long)(r.lat_us.empty() ? 0 : r.lat_us.back()),
                mean, thru, (unsigned long long)r.drops);
    std::fflush(stdout);
}

// ---- perf-gate mode --------------------------------------------------------
// A single stable scenario: closed-loop, single worker (no inter-worker
// scheduling noise), matched load (inflight=1). We take the BEST-OF-R median of
// the emit→result latency — a best-case micro-throughput regression gate for the
// per-frame path, NOT a p99/tail claim (05 guardrail #2). p95/p99/throughput are
// printed for context but NOT emitted as GATE lines (they are too scheduler-noisy
// to gate; the review says report them, gate the median).
static int gate_main() {
    Config cfg;
    cfg.parallel = 1; cfg.inflight = 1; cfg.ordered = false;
    cfg.frames = 5000; cfg.width = 320; cfg.height = 240; cfg.channels = 3;

    // warm up (pool first-touch faults, thread spin-up), then best-of-R.
    run_scenario(cfg);
    int64_t best_p50 = (int64_t)1e18, best_p95 = 0, best_p99 = 0;
    Result keep;
    for (int rep = 0; rep < 7; ++rep) {
        Result r = run_scenario(cfg);
        std::sort(r.lat_us.begin(), r.lat_us.end());
        int64_t p50 = pct(r.lat_us, 50);
        if (p50 < best_p50) { best_p50 = p50; best_p95 = pct(r.lat_us, 95);
                              best_p99 = pct(r.lat_us, 99); keep = std::move(r); }
    }
    // GATED (slower-is-worse): the best-of median steady-state per-frame latency.
    std::printf("GATE hotpath_emit_to_result_p50_us_320x240x3_p1 %lld\n", (long long)best_p50);
    // Informational only (NOT gated): the tail + throughput this scenario saw.
    std::printf("# hotpath best-of-7 closed-loop inflight=1: p95=%lld us  p99=%lld us  "
                "throughput=%.0f frames/s (informational, not gated)\n",
                (long long)best_p95, (long long)best_p99,
                keep.elapsed_s > 0 ? (double)keep.completed / keep.elapsed_s : 0.0);
    xi_perf::print_fingerprint();
    return 0;
}

int main(int argc, char** argv) {
    xi::install_pack_abi();   // wire xi.pack@1 builder/accessors + the bus pack releaser

    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](int def) { return i + 1 < argc ? std::atoi(argv[++i]) : def; };
        if      (a == "--gate")      return gate_main();
        else if (a == "--parallel")  cfg.parallel = next(cfg.parallel);
        else if (a == "--frames")    cfg.frames   = next(cfg.frames);
        else if (a == "--inflight")  cfg.inflight = next(cfg.inflight);
        else if (a == "--pace-us")   cfg.pace_us  = next(cfg.pace_us);
        else if (a == "--work")      cfg.work     = next(cfg.work);
        else if (a == "--width")     cfg.width    = next(cfg.width);
        else if (a == "--height")    cfg.height   = next(cfg.height);
        else if (a == "--channels")  cfg.channels = next(cfg.channels);
        else if (a == "--queue-depth") cfg.queue_depth = next(cfg.queue_depth);
        else if (a == "--ordered")   cfg.ordered  = true;
    }

    std::printf("Per-frame hot path — SPAN: emit -> trigger bus -> dispatch lane -> tiny inspect "
                "-> ordered result.\n");
    std::printf("Frame %dx%dx%d rides an xi.pack@1 pack, fixed inspect touch=%d px. Latency is "
                "steady µs (emit -> ordered-result).\n",
                cfg.width, cfg.height, cfg.channels, cfg.work);
    std::printf("REAL: TriggerBus(emit_pack)/ImagePool/PackRegistry/EmitGate/MetricsRegistry.  "
                "STUBBED: lane queue plumbing (MiniLane).  NOT in span: script compute, "
                "JPEG/expose, WS/PLC.\n\n");

    if (cfg.parallel != 1 || cfg.frames != 20000 || cfg.inflight || cfg.pace_us) {
        // Explicit single-scenario run from the CLI knobs.
        Result r = run_scenario(cfg);
        std::printf("scenario p=%d inflight=%d pace=%dus ordered=%d frames=%d:\n",
                    cfg.parallel, cfg.inflight, cfg.pace_us, cfg.ordered ? 1 : 0, cfg.frames);
        char lbl[64]; std::snprintf(lbl, sizeof(lbl), "p=%d", cfg.parallel);
        print_dist(lbl, r);
        return 0;
    }

    // Default report: a small matrix that separates STEADY-STATE service latency
    // (closed-loop) from OVERLOAD tails (open-loop burst), across parallelism.
    std::printf("--- closed-loop (matched load, inflight=parallel) — STEADY-STATE service latency ---\n");
    for (int p : {1, 2, 4, 8}) {
        Config c = cfg; c.parallel = p; c.inflight = p; c.ordered = (p > 1);
        c.frames = 20000;
        Result r = run_scenario(c);
        char lbl[48]; std::snprintf(lbl, sizeof(lbl), "inflight=parallel=%d%s", p, p > 1 ? " ordered" : "");
        print_dist(lbl, r);
    }
    // Overload demo: a heavier inspect (so compute > frame inter-arrival) offered
    // open-loop into a SHALLOW queue on ONE worker. Now the worker genuinely falls
    // behind: queue wait dominates the tail and the drop policy sheds frames — the
    // finding #2 (best-of hides tails) / #9,#18 (queue-wait + head-of-line) lesson.
    // (With the DEFAULT tiny inspect a single producer's create+memset is slower
    // than the touch, so the worker keeps up and nothing overloads — which is why
    // that flat case is NOT shown as an "overload".)
    std::printf("--- open-loop OVERLOAD (heavy inspect > arrival, shallow queue, 1 worker) — "
                "queue-wait tail + drops ---\n");
    {
        Config c = cfg; c.parallel = 1; c.inflight = 0; c.pace_us = 0;
        c.work = 200000; c.queue_depth = 64; c.frames = 8000;
        Result r = run_scenario(c);
        print_dist("heavy inspect, qd=64", r);
        std::printf("    ^ p50 is small but p99/max are huge = queue wait; drops = the overflow "
                    "policy shedding load. This is\n      what a best-of micro-gate would hide "
                    "(05 review, findings #2, #9, #18).\n");
    }

    std::printf("\nThe closed-loop p50 is the per-frame SERVICE cost; the overload p99/max/drops "
                "show how queue wait +\nhead-of-line blocking inflate the tail once compute can't "
                "keep up — different questions (05 review #2, #18).\n");
    return 0;
}
