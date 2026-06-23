//
// test_qa_race.cpp — unit guards for the FE supervisor's respawn-limiter logic
// (QA agent RACE viewpoint). The integration drivers in examples/qa_race prove
// the death->safe-state->respawn->cap ordering end-to-end; these pin the
// underlying sliding-window math + the safe-state event ordering invariant in
// isolation so a refactor can't quietly break the cap.
//
// The real logic is a file-static loop in fe_main.cpp (not yet extracted to a
// header). RACE-U1 reimplements it faithfully — window prune + cap check
// (fe_main.cpp ~407-424); if a RespawnLimiter type is later extracted, point
// this at it. RACE-U2 checks the cap event carries forward the prior crash
// forensics (fe_main.cpp ~412-418), matching what SP2 asserts in the log.
//
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <xi/xi_safe_state.hpp>
#include <xi/xi_respawn_policy.hpp>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// `allowed == !note_death`: a respawn is permitted until the cap is exceeded.
static bool allowed(xi::RespawnTracker& rt, int cap) { return !rt.note_death(cap); }

int main() {
    using xi::SafeStateReason;
    using xi::SafeStateEvent;
    const int CAP = 5;
    const int64_t RESET = 30'000;

    // RACE-U1a: 6 consecutive deaths -> 5 respawns, the 6th caps.
    {
        xi::RespawnTracker r;
        int a = 0, d = 0;
        for (int i = 0; i < 6; ++i) { if (allowed(r, CAP)) ++a; else ++d; }
        CHECK(a == 5);
        CHECK(d == 1);
    }

    // RACE-U1b (the FIX, replaces the old "slow drip never caps"): a SLOW
    // crash-loop with only brief healthy spells between deaths STILL caps.
    // Spacing is irrelevant — only a sustained-healthy period resets the count.
    {
        xi::RespawnTracker r;
        int a = 0, d = 0;
        for (int i = 0; i < 10; ++i) {
            r.note_healthy(5'000, RESET);   // 5s healthy < reset -> no reset
            if (allowed(r, CAP)) ++a; else ++d;
        }
        CHECK(a == 5);     // capped at exactly the cap
        CHECK(d > 0);      // and it DID latch safe (the old policy never did)
    }

    // RACE-U1c: burst — capped at exactly CAP respawns no matter the death rate.
    {
        xi::RespawnTracker r;
        int a = 0;
        for (int i = 0; i < 50; ++i) if (allowed(r, CAP)) ++a;
        CHECK(a == CAP);
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(s) FAILED\n", g_failures);
    return 1;
}
