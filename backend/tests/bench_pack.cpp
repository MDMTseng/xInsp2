//
// bench_pack.cpp — the v3 uniform keyed-buffer PACK plane on the per-pack hot
// path. Measures the four costs the plane exists to lower, in ABSOLUTE ns/op.
//
// POST-CUT (v11→v12 ABI break): this was a "Pack (v3) vs Record (v2 shared yyjson
// doc)" side-by-side. THE CUT deleted xi::Record, DocRegistry, and the host
// doc_retain/doc_release/doc_refcount slots, so the Record lane can no longer be
// built or measured — the comparison is RETIRED. What remains is a pack-only
// bench of the same build/read/hop/free split; the numbers are absolute, not a
// win/loss against Record. (The old Record baseline lives in git history if the
// comparison is ever needed against a reconstructed v2.)
//
// This is a MEASUREMENT, not an advocacy: if the pack path is slow at a step, the
// number is reported verbatim, with any implementation cause named.
//
// ---------------------------------------------------------------------------
// THE COSTS UNDER TEST (docs/new_gen/07-uniform-keyed-buffer-plane.md, updated
// for the pack-v3 SLAB container — scalars stored RAW, canonical msgpack only
// at the serialization edge via canonical_value):
//   C1  slab bump BUILD (staging scratch + one slab write at seal)         (§Costs)
//   C2  one-shot pack FREE (slab back to the per-thread recycle pool)      (§lifecycle 4)
//   C3  sealed RAW READS (binary search + one raw 8-byte load, no decode)  (§profile 1)
//   C4  move-on-HOP (a sealed Pack hops as a MOVE — pointer swap, no copy,
//       no refcount CAS)                                                   (§D "small plane")
// The micros below isolate those costs at the same points.
//
// SLAB MIGRATION NOTE (packv3, d8fe140): the previous revision of this bench
// modelled the small plane as ONE contiguous canonical-msgpack buffer
// (xi::mp::Writer), hopped it by memcpy, and read M fields by PRECOMPUTED
// OFFSET (pack_mp_detail fixed-width decodes). That representation is retired:
// nothing in production builds an mp plane to hop a pack or reads mp-at-offset
// from a Pack anymore (the fixed-offset readers' remaining consumers decode
// WIRE bytes, covered by bench_pack_c). The old "mp plane + memcpy-hop +
// offset-read" lanes were therefore replaced by their slab equivalents: the
// dispatch event carries a sealed xi::Pack (moved, never copied) and the
// consumer reads M fields through the typed accessors (raw slab loads).
//
// ---------------------------------------------------------------------------
// THE PACK PLANE ------------------------------------------------------------
//
//   Per pack the producer builds N scalars + a nested msgpack map through
//   xi::PackBuilder (thread-local staging scratch, zero steady-state heap,
//   C1), seals to ONE contiguous slab, and the event carries the sealed Pack
//   BY MOVE (C4 — ownership transfer is a pointer swap). The consumer READS M
//   fields via the typed accessors: a binary search on the hash-sorted
//   directory + a raw 8-byte load, zero msgpack decode (C3). FREE returns the
//   slab to the per-thread recycle pool in one shot (C2).
//
//   IMAGE traffic in the dispatch part is REAL: a pooled 320x240x3 frame
//   (xi::ImagePool) rides the event, addref'd on emit, released on consume, and
//   touched by the same fixed tiny inspect. (07 §D1 keeps large buffers as pool
//   handles; only the SMALL plane's ownership discipline is what 07 changes.)
//
// ---------------------------------------------------------------------------
// WHAT IS REAL vs MODELLED (truth-in-labeling):
//   REAL:     ImagePool (pooled frames, addref/release), EmitGate/EmitTurn
//             (arrival-ordered result emission), xi_pack.hpp PackBuilder/Pack +
//             xi_mp.hpp Writer/Reader (the actual v3 code from tasks 1a/1b).
//   MODELLED: the dispatch lane's queue plumbing is MiniLane — the same faithful
//             GroupLane stand-in bench_hotpath uses (deque+mutex+cv, arrival_id,
//             EmitTurn). The producer enqueues directly into MiniLane (this bench
//             exercises the xi_pack.hpp/xi_mp.hpp container + plane primitives,
//             not the TriggerBus emit_pack path — bench_hotpath covers that).
//   NOT in span (on purpose): script/plugin compute (a fixed tiny inspect stands
//             in), JPEG/expose, WS/PLC — same exclusions as bench_hotpath.
//
#include "perf_fingerprint.hpp"

#include "xi/xi_clock.hpp"
#include "xi/xi_emit_gate.hpp"
#include "xi/xi_pack.hpp"
#include "xi/xi_image.hpp"
#include "xi/xi_image_pool.hpp"
#include "xi/xi_metrics.hpp"
#include "xi/xi_mp.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;

// Stop the optimizer eliding the (tiny) inspect + the metadata reads.
static volatile uint64_t g_sink = 0;

// The metadata-plane shape, held fixed across BOTH lanes so the comparison is
// apples-to-apples. A representative inspection-result header: N scalar fields
// (mixed int/float/str/bool) + a small nested structure (a region descriptor).
// M of the scalars are read back by the consumer via typed accessors.
static constexpr int kScalars = 8;   // $src, seq, count, ts_us, score, x, y, pass
static constexpr int kReads   = 4;   // seq, score, count, $src  (the consumer's M)

// ---------------------------------------------------------------------------
// PACK lane metadata — build the same N scalars + nested map as a sealed slab
// Pack through xi::PackBuilder. Scalars land RAW in the slab (one 8-byte store,
// no msgpack encode); the nested region is one canonical-msgpack Mp entry
// (nesting is msgpack's job, D3).
// ---------------------------------------------------------------------------
static xi::Pack build_bench_pack(int64_t seq) {
    xi::PackBuilder b;
    b.add_str("$src", "matcher");
    b.add_i64("seq", seq);
    b.add_i64("count", seq % 17);
    b.add_i64("ts_us", seq * 1000);
    b.add_f64("score", 0.7 + (seq % 30) * 0.01);
    b.add_f64("x", 100.0 + seq * 0.5);
    b.add_f64("y", 50.0 + seq * 0.25);
    b.add_bool("pass", (seq & 1) != 0);
    xi::mp::Writer w;
    w.map(4);
    w.key("area");  w.float_(142.5 + (double)seq);
    w.key("cx");    w.float_(12.0);
    w.key("cy");    w.float_(34.0);
    w.key("label"); w.str("ok");
    xi::mp::Bytes region = w.take();
    b.add_mp("region", region.data(), region.size());
    return b.seal();
}

// The consumer's M reads on a sealed slab Pack — the crux of C3 post-slab: a
// binary search on the hash-sorted directory + a raw aligned load per field,
// zero msgpack decode. (The old precomputed-offset mp decode this replaces
// measured the retired mp-plane representation; wire-byte offset decodes are
// bench_pack_c's territory.)
static inline void read_bench_pack(const xi::Pack& f) {
    uint64_t s = 0;
    s += (uint64_t)f.get_i64("seq").value_or(0);
    s += (uint64_t)(int64_t)f.get_f64("score").value_or(0);
    s += (uint64_t)f.get_i64("count").value_or(0);
    s += f.get_str("$src").value_or(std::string_view{}).size();
    g_sink += s;
}

// ===========================================================================
// PART 1 — the metadata-plane MICRO (no images, no dispatch): the cleanest read
// on C1/C2/C3/C4, least scheduler noise. Each op is self-contained; best_us takes
// the min over batches (strips scheduler noise), exactly like bench_record.
// ===========================================================================
template <class F>
static double best_us(F&& op, int L = 2000, int R = 60) {
    op();
    double best = 1e30;
    for (int b = 0; b < R; ++b) {
        auto t0 = clk::now();
        for (int i = 0; i < L; ++i) op();
        auto t1 = clk::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / L;
        if (us < best) best = us;
    }
    return best;
}

// (b) PACK container (xi_pack.hpp) — PackBuilder + seal + read M (sealed typed
//     accessors) + drop. The task's named "PackBuilder+seal+read+drop": slab
//     BUILD (C1), sealed raw READ (C3), one-shot FREE (C2). No hop.
//     METRIC MEANING CHANGE (slab migration): the container now stores scalars
//     RAW in one slab (build no longer msgpack-encodes scalars; seal adds a
//     hash-sort + one slab write; reads are raw 8-byte loads via binary
//     search). GATE frame_micro_framebuilder_ns therefore measures the slab
//     build/read/free cost — compare against pre-migration baselines with that
//     in mind.
static double micro_frame_builder() {
    return best_us([&] {
        xi::Pack f = build_bench_pack(7);
        read_bench_pack(f);
        // f drops: slab back to the per-thread recycle pool, no handles.
    });
}

// (c) SLAB RAW READ — M typed reads on one pre-sealed pack, isolating C3 (the
//     per-read cost: directory binary search + raw aligned load, no decode).
//     METRIC RENAME (slab migration): this lane REPLACES the retired
//     frame_micro_plane_memcpy_hop_ns ("mp plane + memcpy-hop + offset-read").
//     That lane modelled the pre-slab representation — a contiguous canonical-
//     msgpack small plane hopped by memcpy and decoded at precomputed offsets —
//     which nothing in production does anymore: a sealed Pack hops as a MOVE
//     (pointer swap, cost ~0) and consumers read raw slab. Old and new numbers
//     are NOT comparable.
static double micro_slab_read() {
    xi::Pack f = build_bench_pack(7);
    return best_us([&] { read_bench_pack(f); });
}

// ===========================================================================
// PART 2 — the DISPATCH path. emit -> funnel -> lane -> tiny inspect -> ordered
// result, real pooled image traffic, per pack a sealed slab Pack built by the
// producer, MOVED onto the event (the post-slab hop), and read by the consumer
// via typed raw-slab accessors.
// ===========================================================================
struct BenchEvent {
    int64_t         timestamp_us   = 0;   // steady t_emit
    int64_t         dequeued_at_us = 0;
    int64_t         arrival_id     = 0;
    xi_image_handle image          = XI_IMAGE_NULL;   // pooled 320x240x3 frame
    xi::Pack        pack;                             // the sealed slab pack (moves)
};

struct MiniLane {
    std::deque<BenchEvent>   q;
    std::mutex               mu;
    std::condition_variable  cv;
    std::vector<std::thread> workers;
    xi::EmitGate             gate;
    bool                     ordered = false;
    std::atomic<int64_t>     seq_next{0};
    std::atomic<int64_t>     run_id{0};
    int                      queue_depth = 1024;
    std::atomic<uint64_t>    dropped{0};
};

struct Config {
    int  width = 320, height = 240, channels = 3;
    int  parallel = 1;
    int  frames = 20000;
    bool ordered = false;
    int  inflight = 0;
    int  work = 256;
    int  queue_depth = 1024;
};

struct Result {
    std::vector<int64_t> lat_us;
    uint64_t drops = 0;
    double   elapsed_s = 0;
    uint64_t completed = 0;
};

static inline void tiny_inspect(const uint8_t* px, size_t nbytes, int work) {
    if (!px || nbytes == 0) return;
    uint64_t acc = 0;
    size_t step = nbytes > (size_t)work ? nbytes / (size_t)work : 1;
    for (size_t i = 0; i < nbytes; i += step) acc += px[i];
    g_sink += acc;
}

static Result run_scenario(const Config& cfg) {
    auto& pool = xi::ImagePool::instance();
    const size_t nbytes = (size_t)cfg.width * cfg.height * cfg.channels;

    MiniLane lane;
    lane.ordered     = cfg.ordered && cfg.parallel > 1;
    lane.queue_depth = cfg.queue_depth;

    std::atomic<bool>     keep_going{true};
    std::atomic<uint64_t> completed{0};
    std::vector<int64_t>  lat((size_t)cfg.frames, -1);

    std::mutex              permit_mu;
    std::condition_variable permit_cv;
    int                     permits = cfg.inflight > 0 ? cfg.inflight : cfg.frames + 1;
    auto release_permit = [&] {
        { std::lock_guard<std::mutex> lk(permit_mu); ++permits; }
        permit_cv.notify_one();
    };

    // ---- workers: dequeue -> inspect -> typed slab reads -> ordered emit -----
    for (int w = 0; w < cfg.parallel; ++w) {
        lane.workers.emplace_back([&] {
            while (keep_going.load()) {
                BenchEvent ev; bool have = false; int64_t rid = 0; int64_t eseq = -1;
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

                ev.dequeued_at_us = xi::wall_us();
                tiny_inspect(pool.data(ev.image), nbytes, cfg.work);

                // --- the metadata reads: the pack already hopped BY MOVE with
                //     the event; read M fields via the typed raw-slab accessors.
                read_bench_pack(ev.pack);

                xi::EmitTurn turn(&lane.gate, eseq, &keep_going);
                turn.wait_turn();
                int64_t now = xi::mono_us();
                lat[(size_t)rid - 1] = now - ev.timestamp_us;
                turn.complete();

                pool.release(ev.image);   // drop the event's addref (the "attach")
                completed.fetch_add(1, std::memory_order_relaxed);
                if (cfg.inflight > 0) release_permit();
            }
        });
    }

    // ---- producer: build the sealed pack + a pooled image, enqueue -----------
    auto enqueue = [&](BenchEvent&& ev) {
        std::unique_lock<std::mutex> lk(lane.mu);
        if ((int)lane.q.size() < lane.queue_depth) {
            ev.arrival_id = lane.run_id.fetch_add(1) + 1;
            lane.q.push_back(std::move(ev));
            lane.cv.notify_one();
        } else {
            // Closed-loop with inflight<=queue_depth never overflows; kept for parity.
            lane.dropped.fetch_add(1, std::memory_order_relaxed);
            pool.release(ev.image);
            lk.unlock();
            if (cfg.inflight > 0) release_permit();
        }
    };

    auto t_start = clk::now();
    for (int i = 0; i < cfg.frames; ++i) {
        if (cfg.inflight > 0) {
            std::unique_lock<std::mutex> lk(permit_mu);
            permit_cv.wait(lk, [&] { return permits > 0; });
            --permits;
        }
        xi_image_handle h = pool.create(cfg.width, cfg.height, cfg.channels);
        pool.addref(h);                 // the event's ref (the "attach" the image rides)
        pool.release(h);                // drop the create ref; event keeps its addref

        BenchEvent ev;
        ev.image        = h;
        ev.timestamp_us = xi::mono_us();
        ev.pack         = build_bench_pack(i);
        enqueue(std::move(ev));
    }

    // ---- drain ----------------------------------------------------------------
    uint64_t expect = (uint64_t)cfg.frames;
    for (;;) {
        if (completed.load() + lane.dropped.load() >= expect) break;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    double elapsed = std::chrono::duration<double>(clk::now() - t_start).count();

    keep_going.store(false);
    { std::lock_guard<std::mutex> lk(lane.mu); }
    lane.cv.notify_all();
    { std::lock_guard<std::mutex> lk(lane.gate.mu); }
    lane.gate.cv.notify_all();
    for (auto& t : lane.workers) t.join();

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
    return v[idx];
}

static void print_dist(const char* label, Result& r) {
    std::sort(r.lat_us.begin(), r.lat_us.end());
    double mean = 0;
    for (int64_t v : r.lat_us) mean += (double)v;
    if (!r.lat_us.empty()) mean /= (double)r.lat_us.size();
    double thru = r.elapsed_s > 0 ? (double)r.completed / r.elapsed_s : 0.0;
    std::printf("  %-30s  n=%-6zu  p50=%6lld  p95=%7lld  p99=%7lld  max=%8lld  mean=%8.1f us  "
                "%9.0f frames/s\n",
                label, r.lat_us.size(),
                (long long)pct(r.lat_us, 50), (long long)pct(r.lat_us, 95),
                (long long)pct(r.lat_us, 99),
                (long long)(r.lat_us.empty() ? 0 : r.lat_us.back()), mean, thru);
    std::fflush(stdout);
}

// Best-of-R median of the closed-loop, single-worker per-pack latency — the
// stable gate signal (matches bench_hotpath's gate discipline).
static int64_t gate_p50() {
    Config cfg;
    cfg.parallel = 1; cfg.inflight = 1; cfg.ordered = false;
    cfg.frames = 5000;
    run_scenario(cfg);   // warm up
    int64_t best_p50 = (int64_t)1e18;
    for (int rep = 0; rep < 7; ++rep) {
        Result r = run_scenario(cfg);
        std::sort(r.lat_us.begin(), r.lat_us.end());
        int64_t p50 = pct(r.lat_us, 50);
        if (p50 < best_p50) best_p50 = p50;
    }
    return best_p50;
}

// ---- perf-gate mode --------------------------------------------------------
// Machine-readable GATE lines (integer, slower-is-worse) for perf_gate.cmake:
// the metadata-micro medians (least noise) + the closed-loop single-worker p50.
// No baseline shipped -> SKIPs-with-reason off the capture box.
static int gate_main() {
    // micro: min-of-batches (ns), the cleanest per-op metadata cost.
    double frb_ns   = micro_frame_builder()      * 1000.0;
    double frr_ns   = micro_slab_read()          * 1000.0;
    std::printf("GATE frame_micro_framebuilder_ns %lld\n",       (long long)(frb_ns + 0.5));
    // RENAMED from frame_micro_plane_memcpy_hop_ns (slab migration — the mp
    // plane + memcpy-hop + offset-read lane measured the retired pre-slab
    // representation; see the lane comment on micro_slab_read).
    std::printf("GATE frame_micro_slab_read_ns %lld\n",          (long long)(frr_ns + 0.5));

    int64_t frm_p50 = gate_p50();
    std::printf("GATE frame_hotpath_frame_p50_us_320x240x3_p1 %lld\n",  (long long)frm_p50);

    xi_perf::print_fingerprint();
    return 0;
}

int main(int argc, char** argv) {
    Config cli;
    bool have_cli = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](int def) { return i + 1 < argc ? std::atoi(argv[++i]) : def; };
        if      (a == "--gate")     return gate_main();
        else if (a == "--parallel") { cli.parallel = next(cli.parallel); have_cli = true; }
        else if (a == "--frames")   { cli.frames   = next(cli.frames);   have_cli = true; }
        else if (a == "--inflight") { cli.inflight = next(cli.inflight); have_cli = true; }
        else if (a == "--work")     { cli.work     = next(cli.work);     have_cli = true; }
        else if (a == "--ordered")  { cli.ordered  = true;              have_cli = true; }
        else if (a == "--pack")     { /* pack is the only lane post-CUT */ have_cli = true; }
    }

    std::printf("Pack (v3 keyed-buffer plane) — per-pack hot path (Record v2 comparison retired at THE CUT).\n");
    std::printf("Metadata plane: %d scalar fields + a nested region; consumer reads %d fields.\n",
                kScalars, kReads);
    std::printf("Image: pooled %dx%dx%d, REAL ImagePool (addref emit / release consume).\n",
                cli.width, cli.height, cli.channels);
    std::printf("REAL: ImagePool/EmitGate + xi_pack.hpp + xi_mp.hpp.\n");
    std::printf("MODELLED: MiniLane queue plumbing (== bench_hotpath); producer = direct enqueue.\n\n");

    if (have_cli) {
        Result r = run_scenario(cli);
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "PACK p=%d inflight=%d", cli.parallel, cli.inflight);
        print_dist(lbl, r);
        return 0;
    }

    // -------- metadata-only micro (cleanest read on the 07 claims) -----------
    std::printf("--- metadata-plane MICRO (no images, no dispatch; min-of-batches, ns/op) ---\n");
    double frb_ns = micro_frame_builder() * 1000.0;
    double frr_ns = micro_slab_read()     * 1000.0;
    std::printf("  %-52s %9.1f ns/op\n", "PACK  PackBuilder+seal+read+drop [v3 slab]", frb_ns);
    std::printf("  %-52s %9.1f ns/op\n", "PACK  M typed raw-slab reads on a sealed pack [v3 read]", frr_ns);
    std::printf("    ^ Slab cost = staged build + hash-sort seal + raw reads + one-shot free;\n");
    std::printf("      Read cost = per-read directory binary search + raw aligned load (no decode).\n\n");

    // -------- dispatch across parallelism ------------------------------------
    std::printf("--- closed-loop dispatch (matched load, inflight=parallel) — STEADY-STATE service latency ---\n");
    for (int p : {1, 2, 4, 8}) {
        Config c; c.parallel = p; c.inflight = p; c.ordered = (p > 1);
        c.frames = 20000;
        Result r = run_scenario(c);
        char lbl[48];
        std::snprintf(lbl, sizeof(lbl), "PACK   inflight=parallel=%d%s", p, p > 1 ? " ordered" : "");
        print_dist(lbl, r);
    }
    std::printf("\nThe closed-loop p50 is the per-pack SERVICE cost. Read the micro for the isolated\n"
                "build/read/hop/free split.\n");
    return 0;
}
