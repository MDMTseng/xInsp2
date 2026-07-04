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
// THE COSTS UNDER TEST (docs/new_gen/07-uniform-keyed-buffer-plane.md):
//   C1  arena bump-alloc BUILD (vs mutable-DOM node allocation)            (§Costs)
//   C2  one-shot pack FREE (vs refcount reconciliation)                   (§lifecycle 4)
//   C3  sealed O(1) offset READS (canonical fixed-width + fixed order)     (§profile 1)
//   C4  memcpy-on-HOP on the small plane (vs a contended refcount CAS)     (§D "small plane")
// The micros below isolate those four costs at the same points.
//
// ---------------------------------------------------------------------------
// THE PACK PLANE ------------------------------------------------------------
//
//   Per pack the producer builds the small plane as ONE contiguous canonical-
//   profile msgpack buffer via xi::mp::Writer — N scalars + a nested map — a
//   single growing arena-style buffer, zero per-node heap allocation (C1). The
//   HOP is a raw std::memcpy of that sealed plane into the consumer's (reused)
//   arena — no refcount, no CAS (C4). The consumer READS M fields by DIRECT
//   OFFSET: the canonical profile fixes every field's byte offset for a known
//   schema, so reads are a fixed-width decode at a precomputed offset (C3). FREE
//   is dropping the plane buffer (C2).
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
// The HOP uses the mp::Writer contiguous plane rather than the xi_pack.hpp
// PackBuilder arena because 07 §D1's "arena copy" hop must be an actual MEMCPY of
// a position-independent small plane; the PackBuilder arena is a scattered chunk
// set whose pointers can't be memcpy'd verbatim. The PackBuilder/Pack container
// (arena bump + seal + O(1) index + one-shot free) IS measured directly in the
// metadata-only micro below, the cleanest read on C1/C2/C3.
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

// The CONTRACT-declared keyset for this metadata shape (the _keys.h key order).
// A schema turns the field names into compile-time SLOTS, so the TypedPack
// container reads a declared field by direct slot index — the offset-accessor
// read path (doc 07 §profile-1). Field order here IS the contract order.
struct BenchSchema : xi::PackSchema<BenchSchema> {
    static constexpr std::array<std::string_view, 9> keys = {
        "$src", "seq", "count", "ts_us", "score", "x", "y", "pass", "region"};
    enum : int { kSrc, kSeq, kCount, kTsUs, kScore, kX, kY, kPass, kRegion };
};

// ---------------------------------------------------------------------------
// PACK lane metadata — build the small plane as ONE canonical msgpack buffer.
// Same N scalars + nested map. mp::Writer emits the canonical max-width profile
// (int64 0xd3, float64 0xcb, str32 0xdb, map32) — the v3 small plane verbatim.
// ---------------------------------------------------------------------------
static xi::mp::Bytes build_pack_plane(int64_t seq) {
    xi::mp::Writer w;
    w.map(kScalars + 1);                                   // 8 scalars + region
    w.key("$src");  w.str("matcher");
    w.key("seq");   w.int_(seq);
    w.key("count"); w.int_(seq % 17);
    w.key("ts_us"); w.int_(seq * 1000);
    w.key("score"); w.float_(0.7 + (seq % 30) * 0.01);
    w.key("x");     w.float_(100.0 + seq * 0.5);
    w.key("y");     w.float_(50.0 + seq * 0.25);
    w.key("pass");  w.boolean((seq & 1) != 0);
    w.key("region"); w.map(4);
        w.key("area");  w.float_(142.5 + (double)seq);
        w.key("cx");    w.float_(12.0);
        w.key("cy");    w.float_(34.0);
        w.key("label"); w.str("ok");
    return w.take();
}

// The precomputed offset table — the crux of C3. Because the profile is
// canonical (fixed-width numbers, widest markers) AND the field order is fixed by
// the schema, each field's VALUE begins at a byte offset that is IDENTICAL across
// every pack. A generated accessor caches these once; every read is then a
// fixed-width decode at a known offset (no scan, no hash). We compute the table
// ONCE from a sample plane by a single structural scan.
struct PackOffsets { size_t seq = 0, score = 0, count = 0, src = 0; bool ok = false; };

static void skip_value_(xi::mp::Reader& r) {
    xi::mp::Element e;
    if (r.next(e) != xi::mp::Status::Ok) return;
    if (e.kind == xi::mp::Kind::Array)
        for (uint32_t i = 0; i < e.len; ++i) skip_value_(r);
    else if (e.kind == xi::mp::Kind::Map)
        for (uint32_t i = 0; i < e.len; ++i) { skip_value_(r); skip_value_(r); }
    // scalars/str/bin/ext are fully consumed by next()
}

static PackOffsets compute_offsets(const xi::mp::Bytes& plane) {
    PackOffsets off;
    xi::mp::Reader r(plane);
    xi::mp::Element top;
    if (r.next(top) != xi::mp::Status::Ok || top.kind != xi::mp::Kind::Map) return off;
    for (uint32_t i = 0; i < top.len; ++i) {
        xi::mp::Element k;
        if (r.next(k) != xi::mp::Status::Ok || k.kind != xi::mp::Kind::Str) return off;
        std::string key((const char*)k.data, k.len);
        size_t voff = r.offset();                 // value begins here
        if      (key == "seq")   off.seq   = voff;
        else if (key == "score") off.score = voff;
        else if (key == "count") off.count = voff;
        else if (key == "$src")  off.src   = voff;
        skip_value_(r);
    }
    off.ok = off.seq && off.score && off.count && off.src;
    return off;
}

// The consumer's M reads by direct offset on the (hopped) plane bytes — reusing
// xi_pack.hpp's canonical readers (0xd3/0xcb/0xdb), the exact decode a sealed
// Pack's typed accessor performs.
static inline void read_pack_plane(const uint8_t* p, const PackOffsets& off) {
    uint64_t s = 0;
    s += (uint64_t)xi::pack_mp_detail::read_i64(p + off.seq);
    s += (uint64_t)(int64_t)xi::pack_mp_detail::read_f64(p + off.score);
    s += (uint64_t)xi::pack_mp_detail::read_i64(p + off.count);
    s += xi::pack_mp_detail::read_str(p + off.src).size();
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

// A small canonical msgpack map for the nested "region" — built once, added to
// the PackBuilder via add_mp (D3: nesting is msgpack's job).
static xi::mp::Bytes region_mp(int64_t seq) {
    xi::mp::Writer w;
    w.map(4);
    w.key("area");  w.float_(142.5 + (double)seq);
    w.key("cx");    w.float_(12.0);
    w.key("cy");    w.float_(34.0);
    w.key("label"); w.str("ok");
    return w.take();
}

// (b) PACK container (xi_pack.hpp) — PackBuilder + seal + read M (O(1) sealed
//     accessors) + drop. The task's named "PackBuilder+seal+read+drop": arena
//     bump BUILD (C1), sealed O(1) READ (C3), one-shot FREE (C2). No hop.
static double micro_frame_builder() {
    return best_us([&] {
        xi::PackBuilder b;
        b.add_str("$src", "matcher");
        b.add_i64("seq", 7);
        b.add_i64("count", 7 % 17);
        b.add_i64("ts_us", 7000);
        b.add_f64("score", 0.7 + 7 * 0.01);
        b.add_f64("x", 100.0 + 7 * 0.5);
        b.add_f64("y", 50.0 + 7 * 0.25);
        b.add_i64("pass", 1);
        xi::mp::Bytes region = region_mp(7);
        b.add_mp("region", region.data(), region.size());
        xi::Pack f = b.seal();
        uint64_t s = 0;
        s += (uint64_t)f.get_i64("seq").value_or(0);
        s += (uint64_t)(int64_t)f.get_f64("score").value_or(0);
        s += (uint64_t)f.get_i64("count").value_or(0);
        s += f.get_str("$src").value_or(std::string_view{}).size();
        g_sink += s;
        // f drops: arena freed in one shot, no handles.
    });
}

// (b') PACK container, TYPED (xi_pack.hpp TypedPack<BenchSchema>) — the
//      OFFSET-ACCESSOR read path (doc 07 §profile-1; the wave-1 exit-gate
//      condition). Same N scalars + nested region, but keys are compile-time
//      SLOTS: set_i64<kSeq> writes canonical bytes and points the slot at them
//      with NO key interned; get_i64<kSeq> is slots_[kSeq]->ptr->decode (no hash,
//      no scan). Arena chunks recycle through the per-thread pool (no per-pack
//      heap chunk). This is the container path the shipped PackBuilder (b) lost
//      with — the three named costs removed.
static double micro_frame_typed() {
    return best_us([&] {
        xi::TypedPackBuilder<BenchSchema> b;
        b.set_str<BenchSchema::kSrc>("matcher");
        b.set_i64<BenchSchema::kSeq>(7);
        b.set_i64<BenchSchema::kCount>(7 % 17);
        b.set_i64<BenchSchema::kTsUs>(7000);
        b.set_f64<BenchSchema::kScore>(0.7 + 7 * 0.01);
        b.set_f64<BenchSchema::kX>(100.0 + 7 * 0.5);
        b.set_f64<BenchSchema::kY>(50.0 + 7 * 0.25);
        b.set_i64<BenchSchema::kPass>(1);
        xi::mp::Bytes region = region_mp(7);
        b.set_mp<BenchSchema::kRegion>(region.data(), region.size());
        xi::TypedPack<BenchSchema> f = b.seal();
        uint64_t s = 0;
        s += (uint64_t)f.get_i64<BenchSchema::kSeq>().value_or(0);
        s += (uint64_t)(int64_t)f.get_f64<BenchSchema::kScore>().value_or(0);
        s += (uint64_t)f.get_i64<BenchSchema::kCount>().value_or(0);
        s += f.get_str<BenchSchema::kSrc>().value_or(std::string_view{}).size();
        g_sink += s;
        // f drops: arena chunk returns to the per-thread pool, no handles.
    });
}

// (c) PACK plane + memcpy HOP: mp::Writer plane (BUILD) + memcpy into a reused
//     consumer arena (HOP, C4) + read M by offset (C3) + drop (C2). The memcpy
//     hop is what a refcount-CAS handshake would otherwise cost on the small plane
//     (the v2 Record share_out/adopt CAS this used to be measured against, retired
//     at THE CUT).
static double micro_pack_plane(const PackOffsets& off) {
    static thread_local std::vector<uint8_t> arena;   // consumer arena, reused
    return best_us([&] {
        xi::mp::Bytes plane = build_pack_plane(7);
        arena.resize(plane.size());
        std::memcpy(arena.data(), plane.data(), plane.size());   // the hop
        read_pack_plane(arena.data(), off);
    });
}

// ===========================================================================
// PART 2 — the DISPATCH path. emit -> funnel -> lane -> tiny inspect -> ordered
// result, real pooled image traffic, per pack the metadata plane built by the
// producer and hopped+read by the consumer.
// ===========================================================================
struct BenchEvent {
    int64_t         timestamp_us   = 0;   // steady t_emit
    int64_t         dequeued_at_us = 0;
    int64_t         arrival_id     = 0;
    xi_image_handle image          = XI_IMAGE_NULL;   // pooled 320x240x3 frame
    xi::mp::Bytes   pack_plane;                       // the sealed small plane
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

static Result run_scenario(const Config& cfg, const PackOffsets& off) {
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

    // ---- workers: dequeue -> inspect -> metadata HOP+read -> ordered emit ----
    for (int w = 0; w < cfg.parallel; ++w) {
        lane.workers.emplace_back([&] {
            std::vector<uint8_t> arena;   // this consumer's reused hop arena
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

                // --- the metadata plane hop: memcpy the sealed small plane into
                //     this consumer's arena, then read M fields by direct offset.
                arena.resize(ev.pack_plane.size());
                std::memcpy(arena.data(), ev.pack_plane.data(), ev.pack_plane.size());
                read_pack_plane(arena.data(), off);

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

    // ---- producer: build the metadata plane + a pooled image, enqueue --------
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
        ev.pack_plane   = build_pack_plane(i);
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
static int64_t gate_p50(const PackOffsets& off) {
    Config cfg;
    cfg.parallel = 1; cfg.inflight = 1; cfg.ordered = false;
    cfg.frames = 5000;
    run_scenario(cfg, off);   // warm up
    int64_t best_p50 = (int64_t)1e18;
    for (int rep = 0; rep < 7; ++rep) {
        Result r = run_scenario(cfg, off);
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
static int gate_main(const PackOffsets& off) {
    // micro: min-of-batches (ns), the cleanest per-op metadata cost.
    double frb_ns   = micro_frame_builder()       * 1000.0;
    double frt_ns   = micro_frame_typed()         * 1000.0;
    double frp_ns   = micro_pack_plane(off)      * 1000.0;
    std::printf("GATE frame_micro_framebuilder_ns %lld\n",       (long long)(frb_ns + 0.5));
    std::printf("GATE frame_micro_typed_ns %lld\n",              (long long)(frt_ns + 0.5));
    std::printf("GATE frame_micro_plane_memcpy_hop_ns %lld\n",   (long long)(frp_ns + 0.5));

    int64_t frm_p50 = gate_p50(off);
    std::printf("GATE frame_hotpath_frame_p50_us_320x240x3_p1 %lld\n",  (long long)frm_p50);

    xi_perf::print_fingerprint();
    return 0;
}

int main(int argc, char** argv) {
    PackOffsets off = compute_offsets(build_pack_plane(7));
    if (!off.ok) { std::fprintf(stderr, "pack offset table failed to build\n"); return 2; }

    Config cli;
    bool have_cli = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](int def) { return i + 1 < argc ? std::atoi(argv[++i]) : def; };
        if      (a == "--gate")     return gate_main(off);
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
        Result r = run_scenario(cli, off);
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "PACK p=%d inflight=%d", cli.parallel, cli.inflight);
        print_dist(lbl, r);
        return 0;
    }

    // -------- metadata-only micro (cleanest read on the 07 claims) -----------
    std::printf("--- metadata-plane MICRO (no images, no dispatch; min-of-batches, ns/op) ---\n");
    double frb_ns = micro_frame_builder()  * 1000.0;
    double frt_ns = micro_frame_typed()    * 1000.0;
    double frp_ns = micro_pack_plane(off) * 1000.0;
    std::printf("  %-52s %9.1f ns/op\n", "PACK  PackBuilder+seal+read+drop [v3 dynamic]", frb_ns);
    std::printf("  %-52s %9.1f ns/op\n", "PACK  TypedPack set+seal+slot-read+drop [v3 typed]", frt_ns);
    std::printf("  %-52s %9.1f ns/op\n", "PACK  mp plane + memcpy-hop + offset-read [v3 hop]", frp_ns);
    std::printf("    ^ Dynamic cost = arena build + hybrid index + interned keys + one-shot free;\n");
    std::printf("      Typed cost   = arena build (recycled chunk) + slot offset reads, no intern, no lookup;\n");
    std::printf("      Hop cost     = mp plane build + memcpy hop + offset-read + drop.\n\n");

    // -------- dispatch across parallelism ------------------------------------
    std::printf("--- closed-loop dispatch (matched load, inflight=parallel) — STEADY-STATE service latency ---\n");
    for (int p : {1, 2, 4, 8}) {
        Config c; c.parallel = p; c.inflight = p; c.ordered = (p > 1);
        c.frames = 20000;
        Result r = run_scenario(c, off);
        char lbl[48];
        std::snprintf(lbl, sizeof(lbl), "PACK   inflight=parallel=%d%s", p, p > 1 ? " ordered" : "");
        print_dist(lbl, r);
    }
    std::printf("\nThe closed-loop p50 is the per-pack SERVICE cost. Read the micro for the isolated\n"
                "build/read/hop/free split.\n");
    return 0;
}
