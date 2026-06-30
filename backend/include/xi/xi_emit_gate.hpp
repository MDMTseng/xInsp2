#pragma once
//
// xi_emit_gate.hpp — ordered-emit gate for the dispatch lanes.
//
// In a lane with result_order:"arrival" + max_parallel>1, frames are COMPUTED in
// parallel but their results must be EMITTED in arrival order. Each frame claims a
// sequence number at dequeue (in arrival order, under the lane lock); EmitTurn then
// serializes only the emit: it blocks until it's this seq's turn on the gate, lets
// the emit happen, and advances the cursor so the next frame proceeds.
//
// The sequence is claimed at dequeue but the emit is deep inside run_one_inspection
// after the parallel compute, so EmitTurn is constructed up front (inert — no wait,
// compute stays parallel), wait_turn() blocks right before the emit, complete()
// advances right after, and the DESTRUCTOR is the backstop: if the run returns before
// the emit (any early-return / exception), the dtor still takes this seq's turn in
// order and advances the cursor, so an orphaned seq can never deadlock the lane.
//
// Pure + header-only (the "keep going" signal is injected, not a global), so it's
// unit-testable — see backend/tests/test_emit_gate.cpp.
//
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace xi {

// Cursor + lock + cv for one ordered stream (one per dispatch lane).
struct EmitGate {
    std::mutex              mu;
    std::condition_variable cv;
    int64_t                 next = 0;   // guarded by mu
};

// RAII emit-order gate. emit_seq < 0 (completion mode / cmd:run) is a total no-op.
// `keep_going` is the lane's liveness flag (e.g. &g_continuous): a wait wakes when it
// flips false so a stop can't strand a waiter. nullptr = never stop (tests / always
// ordered).
struct EmitTurn {
    EmitGate*                g_;
    int64_t                  seq_;
    const std::atomic<bool>* keep_going_;
    bool                     on_;
    bool                     waited_   = false;
    bool                     advanced_ = false;

    EmitTurn(EmitGate* gate, int64_t seq, const std::atomic<bool>* keep_going = nullptr)
        : g_(gate), seq_(seq), keep_going_(keep_going), on_(gate && seq >= 0) {}

    // Block until it's this seq's turn (or the lane stopped). Returns true if it is
    // GENUINELY this seq's turn, false if it woke because the lane stopped before its
    // turn arrived — a caller doing an ordered SIDE EFFECT (e.g. flushing staged sink
    // deliveries to a PLC) must skip it on a false, since a stop wakes every parked
    // seq at once and they'd otherwise run out of order / concurrently. Plain wire
    // emits can ignore the result (out-of-order on a stop is harmless). Idempotent:
    // a second call re-reads the verdict under the lock without re-waiting.
    bool wait_turn() {
        if (!on_) return true;                 // completion mode / no gate: always "our turn"
        std::unique_lock<std::mutex> lk(g_->mu);
        if (!waited_) {
            waited_ = true;
            g_->cv.wait(lk, [this] {
                return g_->next == seq_ || (keep_going_ && !keep_going_->load());
            });
        }
        return g_->next == seq_;
    }
    // Advance the cursor so the next seq proceeds. Idempotent. Takes our turn first
    // if wait_turn() wasn't called (an early-return path). Skips the advance if we
    // woke for a stop rather than our turn (matches the lane teardown).
    void complete() {
        if (!on_ || advanced_) return;
        advanced_ = true;
        if (!waited_) wait_turn();
        { std::lock_guard<std::mutex> lk(g_->mu); if (g_->next == seq_) ++g_->next; }
        g_->cv.notify_all();
    }
    ~EmitTurn() { complete(); }   // backstop for every early-return / exception

    EmitTurn(const EmitTurn&) = delete;
    EmitTurn& operator=(const EmitTurn&) = delete;
};

} // namespace xi
