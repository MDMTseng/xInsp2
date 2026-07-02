//
// bench_frame.cpp — Frame (v3 uniform keyed-buffer plane) vs Record (v2 shared
// yyjson doc) on the per-frame hot path. THE wave-1 GO/NO-GO measurement (doc
// docs/new_gen/08 §1d): does the v3 metadata plane BEAT-OR-MATCH the v2 Record
// on the thing production pays PER FRAME?
//
// This is a MEASUREMENT, not an advocacy. If Frame loses anywhere the number is
// reported as a loss, with any implementation cause named — a Frame loss is more
// valuable to the decision than a flattered win (see the HONESTY block at the
// bottom of this header).
//
// ---------------------------------------------------------------------------
// THE CLAIMS UNDER TEST (docs/new_gen/07-uniform-keyed-buffer-plane.md):
//   C1  arena bump-alloc BUILD beats mutable-DOM node allocation           (§Costs)
//   C2  one-shot frame FREE beats refcount reconciliation                  (§lifecycle 4)
//   C3  sealed O(1) offset READS (canonical fixed-width + fixed order)     (§profile 1)
//   C4  memcpy-on-HOP beats contended doc-refcount CAS on the small plane  (§D "small plane")
// Each lane is built to isolate those four costs at the SAME points so the
// side-by-side is apples-to-apples.
//
// ---------------------------------------------------------------------------
// THE TWO LANES (identical everywhere EXCEPT the metadata plane) ------------
//
//   RECORD lane (v2, today):  per frame the producer builds a fresh xi::Record —
//     N scalar fields + a small nested sub-Record (yyjson mutable-DOM node
//     allocation). The cross-instance HOP is the REAL in-process handshake a
//     plugin call takes: Record::share_out (freeze + one CAS-guarded registry
//     enroll + one reserved retain) → Record::adopt_shared (adopt by pointer).
//     The consumer READS M fields via the typed getters (yyjson hash lookups).
//     FREE is the doc dropping through the host DocRegistry refcount.
//
//   FRAME lane (v3, proposed):  per frame the producer builds the small plane as
//     ONE contiguous canonical-profile msgpack buffer via xi::mp::Writer — the
//     same N scalars + nested map — a single growing arena-style buffer, zero
//     per-node heap allocation (C1). The HOP is a raw std::memcpy of that sealed
//     plane into the consumer's (reused) arena — no refcount, no CAS (C4). The
//     consumer READS M fields by DIRECT OFFSET: the canonical profile fixes every
//     field's byte offset for a known schema, so reads are a fixed-width decode at
//     a precomputed offset (C3). FREE is dropping the plane buffer + one pool
//     release (C2).
//
//   IMAGE traffic is IDENTICAL in both lanes and REAL: a pooled 320x240x3 frame
//   (xi::ImagePool) rides the event, addref'd on emit, released on consume, and
//   touched by the same fixed tiny inspect. It is doc-07-neutral — it is NOT the
//   thing 07 changes — so keeping it identical isolates the metadata plane, which
//   IS what 07 changes. (07 §D1 keeps large buffers as pool handles in BOTH
//   worlds; only the SMALL plane's ownership discipline differs.)
//
// ---------------------------------------------------------------------------
// WHAT IS REAL vs MODELLED (truth-in-labeling; these names are quoted in the
// GO/NO-GO writeup):
//   REAL:     ImagePool (pooled frames, addref/release), DocRegistry (the doc
//             refcount the Record hop CASes on), EmitGate/EmitTurn (arrival-
//             ordered result emission), xi_frame.hpp FrameBuilder/Frame + xi_mp.hpp
//             Writer/Reader (the actual v3 code from tasks 1a/1b), Record
//             share_out/adopt_shared (the actual v2 in-process handshake).
//   MODELLED: the dispatch lane's queue plumbing is MiniLane — the same faithful
//             GroupLane stand-in bench_hotpath uses (deque+mutex+cv, arrival_id,
//             EmitTurn). The TriggerBus funnel is reproduced as the enqueue sink
//             (bench_hotpath uses the real bus, but the real bus's emit() signature
//             only carries a yyjson doc — it cannot carry a Frame plane — so BOTH
//             lanes enqueue directly here, identical overhead, doc-07-neutral).
//   NOT in span (on purpose): script/plugin compute (a fixed tiny inspect stands
//             in), JPEG/expose, WS/PLC — same exclusions as bench_hotpath.
//
// The Frame-lane HOP uses the mp::Writer contiguous plane rather than the
// xi_frame.hpp FrameBuilder arena because 07 §D1's "arena copy" hop must be an
// actual MEMCPY of a position-independent small plane (the claim is memcpy-beats-
// CAS); the FrameBuilder arena is a scattered chunk set whose pointers can't be
// memcpy'd verbatim. The FrameBuilder/Frame container (arena bump + seal + O(1)
// index + one-shot free) IS measured directly in the metadata-only micro below,
// which is the cleanest read on C1/C2/C3.
//
// ---------------------------------------------------------------------------
// HONESTY: the workload is fixed and identical across lanes. If Frame's naive
// path loses, that loss is reported verbatim; the ONLY legitimate substitution is
// replacing a KNOWN-STUB bottleneck with the real primitive (this branch already
// has the real xi_mp, so the frame plane is real, not a stub) — tuning the
// workload to flatter a lane is not done.
//
#include "perf_fingerprint.hpp"

#include "xi/xi_clock.hpp"
#include "xi/xi_doc_registry.hpp"
#include "xi/xi_emit_gate.hpp"
#include "xi/xi_frame.hpp"
#include "xi/xi_image.hpp"
#include "xi/xi_image_pool.hpp"
#include "xi/xi_metrics.hpp"
#include "xi/xi_mp.hpp"
#include "xi/xi_record.hpp"
#include "yyjson.h"

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

// ===========================================================================
// Host seam — the REAL DocRegistry + the REAL ImagePool behind the ABI host_api,
// so Record::share_out/adopt_shared CAS the real registry and xi::Image addrefs
// the real pool. (ImagePool::make_host_api already forwards image_*; we add the
// doc_* doors from DocRegistry.)
// ===========================================================================
static xi_host_api make_host() {
    xi_host_api h = xi::ImagePool::make_host_api();
    h.doc_retain  = [](void* d) { xi::DocRegistry::instance().addref((yyjson_mut_doc*)d); };
    h.doc_release = [](void* d) { xi::DocRegistry::instance().release((yyjson_mut_doc*)d); };
    h.doc_refcount = [](void* d) -> int32_t {
        return (int32_t)xi::DocRegistry::instance().refcount((yyjson_mut_doc*)d);
    };
    return h;
}

// ---------------------------------------------------------------------------
// RECORD lane metadata — build a fresh producer Record (N scalars + nested).
// ---------------------------------------------------------------------------
static xi::Record build_record(int64_t seq) {
    xi::Record r;
    r.set("$src", "matcher");
    r.set("seq", (int64_t)seq);
    r.set("count", (int64_t)(seq % 17));
    r.set("ts_us", (int64_t)(seq * 1000));
    r.set("score", 0.7 + (seq % 30) * 0.01);
    r.set("x", 100.0 + seq * 0.5);
    r.set("y", 50.0 + seq * 0.25);
    r.set("pass", (seq & 1) != 0);
    xi::Record region;
    region.set("area", 142.5 + (double)seq);
    region.set("cx", 12.0);
    region.set("cy", 34.0);
    region.set("label", "ok");
    r.set("region", region);
    return r;
}

// The consumer's M typed reads on an adopted Record (yyjson hash lookups).
static inline void read_record(const xi::Record& c) {
    uint64_t s = 0;
    s += (uint64_t)c.get_int64("seq");
    s += (uint64_t)(int64_t)c.get_double("score");
    s += (uint64_t)c.get_int("count");
    s += c.get_string("$src").size();
    g_sink += s;
}

// ---------------------------------------------------------------------------
// FRAME lane metadata — build the small plane as ONE canonical msgpack buffer.
// Same N scalars + nested map. mp::Writer emits the canonical max-width profile
// (int64 0xd3, float64 0xcb, str32 0xdb, map32) — the v3 small plane verbatim.
// ---------------------------------------------------------------------------
static xi::mp::Bytes build_frame_plane(int64_t seq) {
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
// every frame. A generated accessor caches these once; every read is then a
// fixed-width decode at a known offset (no scan, no hash). We compute the table
// ONCE from a sample plane by a single structural scan.
struct FrameOffsets { size_t seq = 0, score = 0, count = 0, src = 0; bool ok = false; };

static void skip_value_(xi::mp::Reader& r) {
    xi::mp::Element e;
    if (r.next(e) != xi::mp::Status::Ok) return;
    if (e.kind == xi::mp::Kind::Array)
        for (uint32_t i = 0; i < e.len; ++i) skip_value_(r);
    else if (e.kind == xi::mp::Kind::Map)
        for (uint32_t i = 0; i < e.len; ++i) { skip_value_(r); skip_value_(r); }
    // scalars/str/bin/ext are fully consumed by next()
}

static FrameOffsets compute_offsets(const xi::mp::Bytes& plane) {
    FrameOffsets off;
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
// xi_frame.hpp's canonical readers (0xd3/0xcb/0xdb), the exact decode a sealed
// Frame's typed accessor performs.
static inline void read_frame_plane(const uint8_t* p, const FrameOffsets& off) {
    uint64_t s = 0;
    s += (uint64_t)xi::frame_mp_detail::read_i64(p + off.seq);
    s += (uint64_t)(int64_t)xi::frame_mp_detail::read_f64(p + off.score);
    s += (uint64_t)xi::frame_mp_detail::read_i64(p + off.count);
    s += xi::frame_mp_detail::read_str(p + off.src).size();
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
// the FrameBuilder via add_mp (D3: nesting is msgpack's job).
static xi::mp::Bytes region_mp(int64_t seq) {
    xi::mp::Writer w;
    w.map(4);
    w.key("area");  w.float_(142.5 + (double)seq);
    w.key("cx");    w.float_(12.0);
    w.key("cy");    w.float_(34.0);
    w.key("label"); w.str("ok");
    return w.take();
}

// (a) RECORD full metadata roundtrip: build (DOM alloc) + share_out + adopt_shared
//     + read M + release. The v2 per-frame metadata cost end to end.
static double micro_record(const xi_host_api& host) {
    return best_us([&] {
        xi::Record r = build_record(7);
        yyjson_mut_doc* d = r.share_out(host.doc_retain, host.doc_release);
        xi::Record c = xi::Record::adopt_shared(d, host.doc_release,
                                                host.doc_refcount(d) > 1);
        read_record(c);
        // r and c drop here: enroll + reserved refs balance, doc freed at rc 0.
    });
}

// (b) FRAME container (xi_frame.hpp) — FrameBuilder + seal + read M (O(1) sealed
//     accessors) + drop. The task's named "FrameBuilder+seal+read+drop": arena
//     bump BUILD (C1), sealed O(1) READ (C3), one-shot FREE (C2). No hop.
static double micro_frame_builder() {
    return best_us([&] {
        xi::FrameBuilder b;
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
        xi::Frame f = b.seal();
        uint64_t s = 0;
        s += (uint64_t)f.get_i64("seq").value_or(0);
        s += (uint64_t)(int64_t)f.get_f64("score").value_or(0);
        s += (uint64_t)f.get_i64("count").value_or(0);
        s += f.get_str("$src").value_or(std::string_view{}).size();
        g_sink += s;
        // f drops: arena freed in one shot, no handles.
    });
}

// (c) FRAME plane + memcpy HOP: mp::Writer plane (BUILD) + memcpy into a reused
//     consumer arena (HOP, C4) + read M by offset (C3) + drop (C2). The direct
//     counterpart to (a)'s share_out/adopt CAS hop — the memcpy-vs-CAS read.
static double micro_frame_plane(const FrameOffsets& off) {
    static thread_local std::vector<uint8_t> arena;   // consumer arena, reused
    return best_us([&] {
        xi::mp::Bytes plane = build_frame_plane(7);
        arena.resize(plane.size());
        std::memcpy(arena.data(), plane.data(), plane.size());   // the hop
        read_frame_plane(arena.data(), off);
    });
}

// ===========================================================================
// PART 2 — the DISPATCH side-by-side. emit -> funnel -> lane -> tiny inspect ->
// ordered result, real pooled image traffic, per frame the metadata plane built
// by the producer and hopped+read by the consumer. Identical harness for both
// lanes; ONLY the metadata plane differs.
// ===========================================================================
enum class Lane { Record, Frame };

struct BenchEvent {
    int64_t         timestamp_us   = 0;   // steady t_emit
    int64_t         dequeued_at_us = 0;
    int64_t         arrival_id     = 0;
    xi_image_handle image          = XI_IMAGE_NULL;   // pooled 320x240x3 frame
    yyjson_mut_doc* rec_doc        = nullptr;         // RECORD lane: share_out'd doc
    xi::mp::Bytes   frame_plane;                      // FRAME lane: sealed small plane
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
    Lane lane = Lane::Record;
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

static Result run_scenario(const Config& cfg, const xi_host_api& host,
                           const FrameOffsets& off) {
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
            std::vector<uint8_t> arena;   // this consumer's reused hop arena (Frame lane)
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

                // --- the metadata plane: the ONLY per-frame difference ---------
                if (cfg.lane == Lane::Record) {
                    // v2 hop: adopt the share_out'd doc by pointer, read M, release.
                    xi::Record c = xi::Record::adopt_shared(ev.rec_doc, host.doc_release,
                                                            /*frozen=*/false);
                    read_record(c);
                    // c drops -> DocRegistry release -> doc freed (producer already gone).
                } else {
                    // v3 hop: memcpy the sealed small plane into this arena, read M.
                    arena.resize(ev.frame_plane.size());
                    std::memcpy(arena.data(), ev.frame_plane.data(), ev.frame_plane.size());
                    read_frame_plane(arena.data(), off);
                }

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
            if (ev.rec_doc) host.doc_release(ev.rec_doc);
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
        if (cfg.lane == Lane::Record) {
            xi::Record r = build_record(i);
            ev.rec_doc = r.share_out(host.doc_retain, host.doc_release);  // reserved ref -> consumer
            // r drops here: its enroll-side ref releases; doc alive on the reserved ref.
        } else {
            ev.frame_plane = build_frame_plane(i);
        }
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

// Best-of-R median of the closed-loop, single-worker per-frame latency for one
// lane — the stable gate signal (matches bench_hotpath's gate discipline).
static int64_t gate_p50(Lane lane, const xi_host_api& host, const FrameOffsets& off) {
    Config cfg;
    cfg.parallel = 1; cfg.inflight = 1; cfg.ordered = false;
    cfg.frames = 5000; cfg.lane = lane;
    run_scenario(cfg, host, off);   // warm up
    int64_t best_p50 = (int64_t)1e18;
    for (int rep = 0; rep < 7; ++rep) {
        Result r = run_scenario(cfg, host, off);
        std::sort(r.lat_us.begin(), r.lat_us.end());
        int64_t p50 = pct(r.lat_us, 50);
        if (p50 < best_p50) best_p50 = p50;
    }
    return best_p50;
}

// ---- perf-gate mode --------------------------------------------------------
// Machine-readable GATE lines (integer, slower-is-worse) for perf_gate.cmake:
// the metadata-micro medians (least noise) + the closed-loop single-worker p50
// for each lane. No baseline shipped -> SKIPs-with-reason off the capture box.
static int gate_main(const xi_host_api& host, const FrameOffsets& off) {
    // micro: min-of-batches (ns), the cleanest per-op metadata cost.
    double rec_ns   = micro_record(host)          * 1000.0;
    double frb_ns   = micro_frame_builder()       * 1000.0;
    double frp_ns   = micro_frame_plane(off)      * 1000.0;
    std::printf("GATE frame_micro_record_roundtrip_ns %lld\n",   (long long)(rec_ns + 0.5));
    std::printf("GATE frame_micro_framebuilder_ns %lld\n",       (long long)(frb_ns + 0.5));
    std::printf("GATE frame_micro_plane_memcpy_hop_ns %lld\n",   (long long)(frp_ns + 0.5));

    int64_t rec_p50 = gate_p50(Lane::Record, host, off);
    int64_t frm_p50 = gate_p50(Lane::Frame,  host, off);
    std::printf("GATE frame_hotpath_record_p50_us_320x240x3_p1 %lld\n", (long long)rec_p50);
    std::printf("GATE frame_hotpath_frame_p50_us_320x240x3_p1 %lld\n",  (long long)frm_p50);

    xi_perf::print_fingerprint();
    return 0;
}

int main(int argc, char** argv) {
    xi_host_api host = make_host();
    FrameOffsets off = compute_offsets(build_frame_plane(7));
    if (!off.ok) { std::fprintf(stderr, "frame offset table failed to build\n"); return 2; }

    Config cli;
    bool have_cli = false;
    Lane cli_lane = Lane::Frame;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](int def) { return i + 1 < argc ? std::atoi(argv[++i]) : def; };
        if      (a == "--gate")     return gate_main(host, off);
        else if (a == "--parallel") { cli.parallel = next(cli.parallel); have_cli = true; }
        else if (a == "--frames")   { cli.frames   = next(cli.frames);   have_cli = true; }
        else if (a == "--inflight") { cli.inflight = next(cli.inflight); have_cli = true; }
        else if (a == "--work")     { cli.work     = next(cli.work);     have_cli = true; }
        else if (a == "--ordered")  { cli.ordered  = true;              have_cli = true; }
        else if (a == "--record")   { cli_lane = Lane::Record;          have_cli = true; }
        else if (a == "--frame")    { cli_lane = Lane::Frame;           have_cli = true; }
    }

    std::printf("Frame (v3 keyed-buffer plane) vs Record (v2 shared doc) — per-frame hot path.\n");
    std::printf("Metadata plane: %d scalar fields + a nested region; consumer reads %d fields.\n",
                kScalars, kReads);
    std::printf("Image: pooled %dx%dx%d, REAL ImagePool, identical both lanes (addref emit / release consume).\n",
                cli.width, cli.height, cli.channels);
    std::printf("REAL: ImagePool/DocRegistry/EmitGate + xi_frame.hpp + xi_mp.hpp + Record share_out/adopt.\n");
    std::printf("MODELLED: MiniLane queue plumbing (== bench_hotpath); funnel = direct enqueue (bus can't carry a Frame).\n\n");

    if (have_cli) {
        cli.lane = cli_lane;
        Result r = run_scenario(cli, host, off);
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "%s p=%d inflight=%d",
                      cli_lane == Lane::Record ? "RECORD" : "FRAME", cli.parallel, cli.inflight);
        print_dist(lbl, r);
        return 0;
    }

    // -------- metadata-only micro (cleanest read on the 07 claims) -----------
    std::printf("--- metadata-plane MICRO (no images, no dispatch; min-of-batches, ns/op) ---\n");
    double rec_ns = micro_record(host)     * 1000.0;
    double frb_ns = micro_frame_builder()  * 1000.0;
    double frp_ns = micro_frame_plane(off) * 1000.0;
    std::printf("  %-52s %9.1f ns/op\n", "RECORD build+share_out+adopt+read+release [v2]", rec_ns);
    std::printf("  %-52s %9.1f ns/op\n", "FRAME  FrameBuilder+seal+read+drop [v3 container]", frb_ns);
    std::printf("  %-52s %9.1f ns/op\n", "FRAME  mp plane + memcpy-hop + offset-read [v3 hop]", frp_ns);
    std::printf("    ^ Record cost = DOM alloc + registry-CAS hop + hash reads + doc free;\n");
    std::printf("      Frame  cost = arena/contiguous build + one-shot free + O(1) offset reads (+ memcpy hop).\n\n");

    // -------- dispatch side-by-side across parallelism -----------------------
    std::printf("--- closed-loop dispatch (matched load, inflight=parallel) — STEADY-STATE service latency ---\n");
    for (int p : {1, 2, 4, 8}) {
        for (Lane ln : {Lane::Record, Lane::Frame}) {
            Config c; c.parallel = p; c.inflight = p; c.ordered = (p > 1);
            c.frames = 20000; c.lane = ln;
            Result r = run_scenario(c, host, off);
            char lbl[48];
            std::snprintf(lbl, sizeof(lbl), "%-6s inflight=parallel=%d%s",
                          ln == Lane::Record ? "RECORD" : "FRAME", p, p > 1 ? " ordered" : "");
            print_dist(lbl, r);
        }
    }
    std::printf("\nThe closed-loop p50 is the per-frame SERVICE cost; the metadata plane is the only\n"
                "difference between the two lanes. Read the micro for the isolated build/read/hop/free split.\n");
    return 0;
}
