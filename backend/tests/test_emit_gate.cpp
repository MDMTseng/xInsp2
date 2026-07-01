//
// test_emit_gate.cpp — concurrency probe for the ordered-emit gate
// (xi::EmitGate / xi::EmitTurn, xi_emit_gate.hpp). This is the dedicated
// race test that header promised and §28 of core_fix_plan.md flags as the
// Tier-1 "dispatch queue + EmitGate" GAP.
//
// The gate is the primitive behind a dispatch lane with
// result_order:"arrival" + max_parallel>1: frames are COMPUTED in parallel
// but their results must be EMITTED in arrival order. This test reproduces
// the exact lane shape — a dispatcher claims a gapless seq under a lock (so
// seq == arrival order), a worker pool computes with jittered latency, and
// EmitTurn serializes only the emit — then asserts the invariants that would
// break under a bad gate:
//
//   1. Ordered emit: N frames computed out-of-order still emit strictly in
//      seq order (0,1,2,...); no emit runs before its predecessor's.
//   2. Dtor backstop: frames that early-return / throw BEFORE the emit (never
//      call wait_turn/complete) must not strand the lane — the EmitTurn dtor
//      takes their turn in order and advances the cursor, so the surviving
//      emits still complete, in order, with no deadlock.
//   3. Stop wakes waiters: when the lane's keep_going flag flips false, every
//      parked EmitTurn must wake (not hang), and wait_turn() must report false
//      (NOT this seq's turn) for the parked seqs so an ordered side effect is
//      correctly skipped on teardown.
//   4. Idempotency + completion mode: double wait_turn()/complete() are safe;
//      emit_seq<0 (cmd:run / completion mode) is a total no-op that always
//      reports "our turn".
//
// No TSan on MSVC (OQ-1); XINSP2_STRESS_SCALE multiplies the frame count so the
// emit_gate_heavy ctest hammers the CAS/cursor/cv paths far harder.
//

#include <xi/xi_emit_gate.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#define CHECK(expr)                                                  \
    do {                                                             \
        if (!(expr)) {                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                 \
                __FILE__, __LINE__, #expr);                          \
            std::abort();                                            \
        }                                                            \
    } while (0)

#define SECTION(name) std::fprintf(stderr, "\n[section] %s\n", name)

static int stress_scale() {
    static int s = [] {
        const char* e = std::getenv("XINSP2_STRESS_SCALE");
        int v = e ? std::atoi(e) : 1;
        return v > 0 ? v : 1;
    }();
    return s;
}

// Deterministic per-seq jitter (no RNG, so failures reproduce): an LCG seeded by
// seq spins a variable number of iterations to shuffle which worker reaches the
// emit first, WITHOUT changing the arrival order the dispatcher already fixed.
static void jitter(int64_t seq) {
    uint64_t x = (uint64_t)seq * 2654435761u + 1442695040888963407ull;
    x ^= x >> 13; x *= 6364136223846793005ull; x ^= x >> 7;
    int spins = (int)(x % 3000);
    volatile int sink = 0;
    for (int i = 0; i < spins; ++i) sink += i;
    if ((x & 7) == 0) std::this_thread::yield();
}

// ---------- A lane: dispatcher claims gapless seq, pool emits ordered ----------
//
// Mirrors GroupLane: `seq_next` under `dispatch_mu` is the arrival-ordered claim;
// EmitTurn(&gate, seq, &keep_going) replays the emit in that order. Each worker
// pulls the next unit of work, claims its seq under the lock (exactly where the
// real dequeue claims it), computes (jitter), then emits under the gate.

struct Lane {
    xi::EmitGate      gate;
    std::atomic<bool> keep_going{true};
    std::mutex        dispatch_mu;             // serializes the seq claim (arrival order)
    int64_t           seq_next = 0;            // guarded by dispatch_mu
    std::atomic<int>  remaining;               // work units left to claim

    // Emit-order verification: `expected` is the seq that MUST emit next. Each
    // genuine emit checks it under emit_mu and bumps it — a single out-of-order
    // emit trips the CHECK.
    std::mutex        emit_mu;
    int64_t           expected = 0;            // guarded by emit_mu
    std::vector<int64_t> emit_log;             // guarded by emit_mu
};

static void test_ordered_emit_under_parallel_compute() {
    SECTION("N frames computed in parallel emit strictly in seq order");
    const int FRAMES  = 20'000 * stress_scale();
    const int WORKERS = 8;

    Lane lane;
    lane.remaining.store(FRAMES, std::memory_order_relaxed);
    lane.emit_log.reserve(FRAMES);

    auto worker = [&] {
        for (;;) {
            // Claim the next unit + its seq under the lane lock, exactly as the
            // real dequeue does: seq follows the order threads acquire the lock.
            int64_t seq;
            {
                std::lock_guard<std::mutex> lk(lane.dispatch_mu);
                if (lane.remaining.load(std::memory_order_relaxed) == 0) break;
                lane.remaining.fetch_sub(1, std::memory_order_relaxed);
                seq = lane.seq_next++;
            }
            // EmitTurn claimed up front (inert), compute stays parallel.
            xi::EmitTurn turn(&lane.gate, seq, &lane.keep_going);
            jitter(seq);
            // Serialize ONLY the emit.
            bool mine = turn.wait_turn();
            CHECK(mine);                        // keep_going never flips here
            {
                std::lock_guard<std::mutex> lk(lane.emit_mu);
                CHECK(lane.expected == seq);    // <-- the ordered-emit invariant
                lane.expected++;
                lane.emit_log.push_back(seq);
            }
            turn.complete();
        }
    };

    std::vector<std::thread> pool;
    for (int i = 0; i < WORKERS; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    // Every frame emitted exactly once, in order, cursor fully drained.
    CHECK((int)lane.emit_log.size() == FRAMES);
    for (int i = 0; i < FRAMES; ++i) CHECK(lane.emit_log[i] == i);
    CHECK(lane.expected == FRAMES);
    std::lock_guard<std::mutex> lk(lane.gate.mu);
    CHECK(lane.gate.next == FRAMES);
}

// ---------- Dtor backstop: early-return seqs must not strand the lane ----------

static void test_dtor_backstop_on_early_return() {
    SECTION("Frames that early-return before emit still advance the cursor");
    const int FRAMES  = 10'000 * stress_scale();
    const int WORKERS = 8;

    Lane lane;
    lane.remaining.store(FRAMES, std::memory_order_relaxed);
    lane.emit_log.reserve(FRAMES);
    std::atomic<int> emitted{0}, bailed{0};

    auto worker = [&] {
        for (;;) {
            int64_t seq;
            {
                std::lock_guard<std::mutex> lk(lane.dispatch_mu);
                if (lane.remaining.load(std::memory_order_relaxed) == 0) break;
                lane.remaining.fetch_sub(1, std::memory_order_relaxed);
                seq = lane.seq_next++;
            }
            xi::EmitTurn turn(&lane.gate, seq, &lane.keep_going);
            jitter(seq);
            // Every 3rd frame simulates an early-return / exception BEFORE the
            // emit: it just drops `turn` here. The dtor is the only thing that
            // advances the cursor for this seq — if it doesn't, later seqs hang.
            if (seq % 3 == 1) { bailed.fetch_add(1, std::memory_order_relaxed); continue; }

            bool mine = turn.wait_turn();
            CHECK(mine);
            {
                std::lock_guard<std::mutex> lk(lane.emit_mu);
                // Emitted seqs are SPARSE here (bailed seqs are skipped), so the
                // invariant is strict monotonicity, not density: seq S only reaches
                // the emit once gate.next==S, i.e. every lower seq (emitted OR
                // dtor-advanced) is already done.
                CHECK(lane.emit_log.empty() || seq > lane.emit_log.back());
                lane.emit_log.push_back(seq);
            }
            emitted.fetch_add(1, std::memory_order_relaxed);
            turn.complete();
        }
    };

    std::vector<std::thread> pool;
    for (int i = 0; i < WORKERS; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();            // <-- would hang forever on a broken backstop

    CHECK(emitted.load() + bailed.load() == FRAMES);
    // The emitted subset is exactly the non-bailed seqs, in order.
    CHECK((int)lane.emit_log.size() == emitted.load());
    for (size_t i = 1; i < lane.emit_log.size(); ++i)
        CHECK(lane.emit_log[i] > lane.emit_log[i-1]);
    for (int64_t s : lane.emit_log) CHECK(s % 3 != 1);
    // Cursor drained to the end regardless of who emitted vs bailed.
    std::lock_guard<std::mutex> lk(lane.gate.mu);
    CHECK(lane.gate.next == FRAMES);
}

// ---------- Stop wakes every parked waiter (lane teardown) ----------

static void test_stop_wakes_parked_waiters() {
    SECTION("keep_going=false wakes all parked EmitTurns; no hang, turn=false");
    const int PARKED = 64;                     // seqs 1..PARKED park behind the hole at 0

    xi::EmitGate gate;
    std::atomic<bool> keep_going{true};

    // Deliberately create a permanent hole: nobody ever completes seq 0, so
    // seqs 1..PARKED block in wait_turn() until the stop flips the flag. This is
    // the teardown case — a stop must wake every parked seq at once.
    std::atomic<int> woke{0};
    std::atomic<int> reported_turn{0};         // how many woke claiming it was THEIR turn
    std::vector<std::thread> parked;
    for (int i = 1; i <= PARKED; ++i) {
        parked.emplace_back([&, seq = (int64_t)i] {
            xi::EmitTurn turn(&gate, seq, &keep_going);
            bool mine = turn.wait_turn();       // blocks: gate.next==0, not our turn
            if (mine) reported_turn.fetch_add(1, std::memory_order_relaxed);
            woke.fetch_add(1, std::memory_order_relaxed);
            // Ordered side effect would be SKIPPED here on !mine — that's the point.
        });
    }

    // Let them all park, then stop the lane.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(woke.load() == 0);                    // still parked (hole at seq 0 never filled)
    keep_going.store(false);
    { std::lock_guard<std::mutex> lk(gate.mu); }
    gate.cv.notify_all();                       // the stop path's wake

    for (auto& t : parked) t.join();            // <-- hangs forever if the stop doesn't wake them
    CHECK(woke.load() == PARKED);
    // None of the parked seqs was ever genuinely its turn (0 was never completed),
    // so every one must have woken reporting turn=false — the skip signal.
    CHECK(reported_turn.load() == 0);
}

// ---------- Idempotency + completion mode ----------

static void test_idempotency_and_completion_mode() {
    SECTION("double wait/complete safe; emit_seq<0 is a no-op");

    // Completion mode: a negative seq is inert — always "our turn", no cursor move.
    {
        xi::EmitGate gate;
        xi::EmitTurn turn(&gate, /*seq=*/-1, nullptr);
        CHECK(turn.wait_turn() == true);
        CHECK(turn.wait_turn() == true);        // idempotent
        turn.complete();
        turn.complete();                        // idempotent
        std::lock_guard<std::mutex> lk(gate.mu);
        CHECK(gate.next == 0);                  // completion mode never advances
    }

    // Ordered mode, single seq: repeated wait/complete must advance exactly once.
    {
        xi::EmitGate gate;
        xi::EmitTurn a(&gate, 0, nullptr);
        CHECK(a.wait_turn() == true);
        CHECK(a.wait_turn() == true);           // re-reads verdict, no re-wait
        a.complete();
        a.complete();                           // no double increment
        std::lock_guard<std::mutex> lk(gate.mu);
        CHECK(gate.next == 1);
    }

    // complete() WITHOUT a prior wait_turn (early-return path) still takes the
    // turn and advances — chained across three seqs in order.
    {
        xi::EmitGate gate;
        { xi::EmitTurn a(&gate, 0, nullptr); a.complete(); }
        { xi::EmitTurn b(&gate, 1, nullptr); b.complete(); }
        { xi::EmitTurn c(&gate, 2, nullptr); /* dtor-only */ }
        std::lock_guard<std::mutex> lk(gate.mu);
        CHECK(gate.next == 3);
    }
}

int main() {
    std::fprintf(stderr, "=== test_emit_gate (scale=%d) ===\n", stress_scale());
    auto t0 = std::chrono::steady_clock::now();

    test_ordered_emit_under_parallel_compute();
    test_dtor_backstop_on_early_return();
    test_stop_wakes_parked_waiters();
    test_idempotency_and_completion_mode();

    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "\nALL TESTS PASSED in %lld ms\n", (long long)dt);
    return 0;
}
