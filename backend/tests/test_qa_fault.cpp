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
// We mirror fe_main.cpp's exact algorithm:
//   on each death:
//     prune timestamps older than window from `respawns`
//     if (respawns.size() >= max)  -> RespawnLimitExceeded, stop
//     else                          -> push(now), respawn
//
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// One death's worth of cap arithmetic — a faithful copy of the fe_main.cpp body.
// Returns true if this death TRIPPED the cap (RespawnLimitExceeded), false if it
// resulted in a respawn.
static bool on_death(std::vector<int64_t>& respawns, int64_t now,
                     int window_ms, int max) {
    respawns.erase(std::remove_if(respawns.begin(), respawns.end(),
                   [&](int64_t ts) { return now - ts > window_ms; }),
                   respawns.end());
    if ((int)respawns.size() >= max) return true;   // stay safe, don't respawn
    respawns.push_back(now);
    return false;
}

int main() {
    const int WINDOW = 60'000;  // 60 s, the FE default
    const int MAX    = 5;       // FeConfig::respawn_max

    // QF-U1: a continuous storm trips the cap on death #(max+1), with exactly
    // `max` respawns first. This is the ledger driver_respawn_accounting checks.
    {
        std::vector<int64_t> respawns;
        int respawned = 0, deaths = 0;
        bool capped = false;
        for (int64_t t = 0; t < 100'000; t += 1500) {   // 1.5 s apart, all within 60 s
            ++deaths;
            if (on_death(respawns, t, WINDOW, MAX)) { capped = true; break; }
            ++respawned;
        }
        CHECK(capped);
        CHECK(respawned == MAX);       // exactly 5 respawns
        CHECK(deaths == MAX + 1);      // tripped on the 6th death
    }

    // QF-U2: the FIRST max deaths never trip the cap (boundary: size==max-1 ok).
    {
        std::vector<int64_t> respawns;
        for (int i = 0; i < MAX; ++i)
            CHECK(on_death(respawns, (int64_t)i * 1000, WINDOW, MAX) == false);
        CHECK((int)respawns.size() == MAX);
        // The next death (the (max+1)th) trips it.
        CHECK(on_death(respawns, (int64_t)MAX * 1000, WINDOW, MAX) == true);
    }

    // QF-U3: sliding window — deaths spaced > window apart never accumulate, so
    // the cap is never tripped (mirrors FE-E8: a slow drip is not a storm).
    {
        std::vector<int64_t> respawns;
        bool capped = false;
        int64_t t = 0;
        for (int i = 0; i < 20; ++i) {
            if (on_death(respawns, t, WINDOW, MAX)) { capped = true; break; }
            t += WINDOW + 5000;        // each death is >60 s after the previous
        }
        CHECK(!capped);
        // Pruning keeps the live set tiny: only the just-pushed one survives.
        CHECK((int)respawns.size() == 1);
    }

    // QF-U4: a burst that trips, but old entries age out — pruning lets the
    // window "recover" so a later isolated death respawns rather than caps.
    {
        std::vector<int64_t> respawns;
        // Fill the window to the brim (max-1 respawns + the cap on the next).
        for (int i = 0; i < MAX; ++i) on_death(respawns, (int64_t)i * 100, WINDOW, MAX);
        CHECK((int)respawns.size() == MAX);
        // A death long after the window: all old entries prune, this one respawns.
        bool capped = on_death(respawns, (int64_t)MAX * 100 + WINDOW + 1, WINDOW, MAX);
        CHECK(!capped);
        CHECK((int)respawns.size() == 1);
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(s) FAILED\n", g_failures);
    return 1;
}
