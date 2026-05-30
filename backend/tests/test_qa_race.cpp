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

// Delegates to the REAL decision (xi::respawn_should_trip, the function
// fe_main.cpp calls). Returns true if a respawn is permitted at time t (and
// records it); false if the cap is exceeded (caller would drive
// RespawnLimitExceeded and stay safe). `respawn_allowed == !should_trip`.
static bool respawn_allowed(std::vector<int64_t>& respawns, int64_t t,
                            int window_s, int respawn_max) {
    return !xi::respawn_should_trip(respawns, t, window_s * 1000, respawn_max);
}

int main() {
    using xi::SafeStateReason;
    using xi::SafeStateEvent;

    // RACE-U1a: 5 deaths inside a 60s window -> the 6th trips the cap.
    {
        std::vector<int64_t> r;
        const int window_s = 60, cap = 5;
        int64_t t = 1'000'000;
        int allowed = 0, denied = 0;
        // Six rapid deaths, 1.5s apart (well inside the 60s window).
        for (int i = 0; i < 6; ++i) {
            if (respawn_allowed(r, t, window_s, cap)) ++allowed; else ++denied;
            t += 1500;
        }
        CHECK(allowed == 5);   // exactly cap respawns permitted
        CHECK(denied == 1);    // the 6th death stays safe (RespawnLimitExceeded)
    }

    // RACE-U1b (mirrors FE-E8): deaths spaced > window apart never trip the cap.
    {
        std::vector<int64_t> r;
        const int window_s = 60, cap = 5;
        int64_t t = 1'000'000;
        int allowed = 0, denied = 0;
        for (int i = 0; i < 10; ++i) {
            if (respawn_allowed(r, t, window_s, cap)) ++allowed; else ++denied;
            t += 61'000;  // 61s apart -> previous death always pruned first
        }
        CHECK(allowed == 10);  // every respawn permitted; window never fills
        CHECK(denied == 0);
        CHECK((int)r.size() <= 1);  // window holds at most the current death
    }

    // RACE-U1c: a burst that exceeds the cap is capped at exactly respawn_max
    // respawns regardless of how many deaths arrive (the BURST property).
    {
        std::vector<int64_t> r;
        const int window_s = 60, cap = 5;
        int64_t t = 2'000'000;
        int allowed = 0;
        for (int i = 0; i < 50; ++i) {       // 50 near-instant deaths
            if (respawn_allowed(r, t, window_s, cap)) ++allowed;
            t += 10;                          // 10ms apart — faster than backoff
        }
        CHECK(allowed == cap);  // never more than the cap, no matter the rate
    }

    // RACE-U2: the cap event carries forward the prior BackendExit forensics
    // (module/report), exactly as fe_main.cpp builds `stuck` from `ev`.
    {
        SafeStateEvent ev;
        ev.reason          = SafeStateReason::BackendExit;
        ev.faulting_module = "raw_thread_crash.dll";
        ev.last_phase      = "inspect";
        ev.report_path     = "C:\\tmp\\xinsp-backend-1-2.json";

        SafeStateEvent stuck;
        stuck.reason          = SafeStateReason::RespawnLimitExceeded;
        stuck.exception_name  = ev.exception_name;
        stuck.faulting_module = ev.faulting_module;
        stuck.report_path     = ev.report_path;

        CHECK(stuck.reason == SafeStateReason::RespawnLimitExceeded);
        CHECK(stuck.faulting_module == "raw_thread_crash.dll");
        CHECK(stuck.report_path == ev.report_path);
        // The cap line should still name the module an operator must fix.
        std::string s;
        {
            std::FILE* f = std::tmpfile();
            xi::LoggingSafeStateSink sink(f);
            sink.enter_safe_state(stuck);
            std::fflush(f); std::rewind(f);
            char buf[1024]; size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
            std::fclose(f);
        }
        CHECK(s.find("reason=RespawnLimitExceeded") != std::string::npos);
        CHECK(s.find("module=raw_thread_crash.dll") != std::string::npos);
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(s) FAILED\n", g_failures);
    return 1;
}
