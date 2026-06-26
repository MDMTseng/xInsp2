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
    // Run `fn` on a detached thread, holding the in-flight count until it returns.
    // Returns false (running nothing) if we're tearing down or the thread couldn't
    // be created — the caller then does its own bail cleanup. Ordering is a correct
    // Dekker handshake with the drain: launch() bumps THEN reads the flag, while
    // begin_shutdown() sets the flag THEN drain() reads the count — so a launch
    // racing teardown is either counted by the drain or bails out.
    template <class Fn>
    bool launch(Fn&& fn) {
        count_.fetch_add(1);
        if (shutting_.load()) { count_.fetch_sub(1); return false; }
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
    // source emit bails instead of launching).
    void begin_shutdown() { shutting_.store(true); }
    bool shutting_down() const { return shutting_.load(); }

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
};

} // namespace xi
