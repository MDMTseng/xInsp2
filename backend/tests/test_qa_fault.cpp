//
// test_qa_fault.cpp — QA-FAULT unit: the FE supervisor's respawn-cap arithmetic.
//
// The cap logic in fe_main.cpp (sliding window + "respawns.size() >= max ->
// stay safe") is a file-static loop body, not callable in isolation. The
// example driver examples/qa_fault/driver_respawn_accounting.py asserts the
// resulting LEDGER end-to-end (N deaths -> N BackendExit + 1 RespawnLimit), but
// that is a slow, spawn-heavy integration test. This unit pins the same
// decision as fast, portable, pure logic so a refactor of the window math is
// caught in ctest before the integration run.
//
// This now tests the REAL decision function fe_main.cpp uses
// (xi::respawn_should_trip in xi/xi_respawn_policy.hpp), not a copy — so a
// refactor of the window math is caught here, not just end-to-end.
//
#include <cstdint>
#include <cstdio>
#include <vector>

#include <xi/xi_respawn_policy.hpp>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

int main() {
    const int MAX = 5;            // FeConfig::respawn_max
    const int64_t RESET = 30'000; // FeConfig::respawn_reset_ms

    // QF-U1: a continuous storm trips the cap on death #(max+1), with exactly
    // `max` respawns first. (driver_respawn_accounting checks the same ledger.)
    {
        xi::RespawnTracker r;
        int respawned = 0, deaths = 0;
        bool capped = false;
        for (int i = 0; i < 100; ++i) {
            ++deaths;
            if (r.note_death(MAX)) { capped = true; break; }
            ++respawned;
        }
        CHECK(capped);
        CHECK(respawned == MAX);       // exactly 5 respawns
        CHECK(deaths == MAX + 1);      // tripped on the 6th death
    }

    // QF-U2: boundary — the first max deaths respawn, the (max+1)th trips.
    {
        xi::RespawnTracker r;
        for (int i = 0; i < MAX; ++i) CHECK(r.note_death(MAX) == false);
        CHECK(r.note_death(MAX) == true);
    }

    // QF-U3 (the FIX): a SLOW crash-loop still caps. Deaths accumulate no matter
    // how far apart, because only a SUSTAINED-healthy period resets the counter —
    // a brief healthy spell between deaths does not. (The old time-window cap
    // missed this and thrashed forever.)
    {
        xi::RespawnTracker r;
        bool capped = false;
        for (int i = 0; i < 20; ++i) {
            r.note_healthy(5'000, RESET);   // only 5s healthy (< reset) -> no reset
            if (r.note_death(MAX)) { capped = true; break; }
        }
        CHECK(capped);
    }

    // QF-U4: a SUSTAINED-healthy period (>= reset) means the line recovered, so
    // prior failures are forgotten and a later death respawns rather than caps.
    {
        xi::RespawnTracker r;
        for (int i = 0; i < MAX; ++i) CHECK(r.note_death(MAX) == false);  // at the brink
        r.note_healthy(RESET, RESET);       // recovered
        CHECK(r.consecutive == 0);
        CHECK(r.note_death(MAX) == false);  // fresh death respawns, doesn't cap
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(s) FAILED\n", g_failures);
    return 1;
}
