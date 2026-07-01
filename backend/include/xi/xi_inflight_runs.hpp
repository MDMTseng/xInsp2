#pragma once
//
// xi_inflight_runs.hpp — structural owner for detached one-shot / cmd:run inspect
// threads.
//
// Each such thread runs an inspect against the main-local `srv` and can OUTLIVE
// the handler that launched it (a source plugin's emit thread launches one-shots),
// so process teardown MUST wait for them before `srv` is destroyed. A bare
// g_run_mu acquire is not enough: it only waits for a thread that already HOLDS
// the lock; one detached-but-not-yet-locked would slip past and then touch the
// about-to-be-destroyed srv — that was the "shutdown-window UAF" the review rounds
// kept hitting.
//
// The protocol (bump BEFORE detach, bail if shutting down, RAII-decrement when the
// thread finishes, drain on teardown) used to be hand-copied at every launch site
// AND in teardown, and a single missed bump/guard there WAS the UAF. Collecting it
// in this one type makes a new launch site a single launch() call that cannot get
// the ordering wrong.
//
// Pure + portable (no Win32, no project deps), so it's unit-testable on its own —
// see backend/tests/test_xi_core.cpp.
//
#include <atomic>
#include <chrono>
#include <system_error>
#include <thread>
#include <utility>

namespace xi {

class InflightRuns {
public:
    // Default max concurrent in-flight detached runs (B1). One-shot inspects
    // serialize on g_run_mu (only one runs at a time), so this bounds the BACKLOG
    // of not-yet-run events, each pinning ImagePool refs + a meta doc. 64 keeps
    // that backlog a small fraction of the 65536 ImagePool cap while still
    // absorbing a reasonable burst — far below the "thread/OOM" runaway a fast
    // source (200fps in, 50fps drain) would otherwise produce.
    static constexpr int kDefaultCap = 64;

    // Configure the max-in-flight ceiling (B1, project.json parallelism.max_inflight).
    // <= 0 means "use the default" (NOT unlimited) — an unbounded launch path is the
    // very runaway this cap exists to prevent.
    void set_cap(int c) { cap_.store(c > 0 ? c : kDefaultCap); }
    int  cap() const { return cap_.load(); }

    // Run `fn` on a detached thread, holding the in-flight count until it returns.
    // Returns false (running nothing) if we're tearing down, we're at the in-flight
    // cap (drop-newest — *dropped_over_cap is set so the caller can emit the
    // XI_SYS_DROPPED marker + bump the dropped counter), or the thread couldn't be
    // created — the caller then does its own bail cleanup. Ordering is a correct
    // Dekker handshake with the drain: launch() bumps THEN reads the flag, while
    // begin_shutdown() sets the flag THEN drain() reads the count — so a launch
    // racing teardown is either counted by the drain or bails out.
    template <class Fn>
    bool launch(Fn&& fn, bool* dropped_over_cap = nullptr) {
        if (dropped_over_cap) *dropped_over_cap = false;
        // Atomically claim an in-flight slot, but only while under the cap. A plain
        // fetch_add-then-check would be a check-then-act race: N launches racing at
        // count_==cap-1 would each bump past the ceiling before any observed it. The
        // CAS loop guarantees count_ never exceeds cap_.
        const int cap = cap_.load();
        int cur = count_.load();
        for (;;) {
            if (cap > 0 && cur >= cap) {          // at the ceiling → drop-newest
                if (dropped_over_cap) *dropped_over_cap = true;
                return false;
            }
            if (count_.compare_exchange_weak(cur, cur + 1)) break;  // slot claimed (cur reloaded on failure)
        }
        if (shutting_.load() || paused_.load() > 0) { count_.fetch_sub(1); return false; }
        try {
            std::thread([this, fn = std::forward<Fn>(fn)]() mutable {
                struct Guard { InflightRuns* s; ~Guard() { s->count_.fetch_sub(1); } } guard{this};
                fn();
            }).detach();
            return true;
        } catch (const std::system_error&) {
            count_.fetch_sub(1);   // thread never started; the Guard never ran
            return false;
        }
    }

    // Teardown: refuse new launches (call BEFORE dropping the emit sink so a late
    // source emit bails instead of launching). TERMINAL — never reversed.
    void begin_shutdown() { shutting_.store(true); }
    bool shutting_down() const { return shutting_.load(); }

    // REVERSIBLE launch pause for a lifecycle op that FreeLibrary's plugin DLLs
    // (close/open/recompile/rebuild/export). Same Dekker handshake as begin_shutdown
    // — pause() sets the flag, then drain() reads the count, so a source emit racing
    // the op either bumps-then-bails or is waited out — but un-doable and nestable
    // (counter), and kept DISTINCT from shutting_ so a lifecycle unpause can never
    // clear a terminal process-shutdown. Pair pause()+drain() before the DLL unload;
    // unpause() after (the lifecycle guard owns this).
    void pause()   { paused_.fetch_add(1); }
    void unpause() { paused_.fetch_sub(1); }

    // Number of detached runs that have been launched but not yet finished.
    int  inflight() const { return count_.load(); }

    // Wait out the in-flight runs, capped (default ~50 s) so a wedged inspect can't
    // hang process exit. Returns true if it drained to zero, false if it hit the cap.
    bool drain(int cap_ms = 50000) {
        for (int i = 0; count_.load() != 0 && i < cap_ms; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return count_.load() == 0;
    }

private:
    std::atomic<int>  count_{0};
    std::atomic<bool> shutting_{false};
    std::atomic<int>  paused_{0};      // reversible lifecycle pause (see pause())
    std::atomic<int>  cap_{kDefaultCap};  // max concurrent in-flight (B1; set_cap)
};

} // namespace xi
