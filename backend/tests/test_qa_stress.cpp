//
// test_qa_stress.cpp — Phase G stress/fuzz units for the FE supervisor's
// safety core (RespawnTracker + SafeStateEvent/LoggingSafeStateSink).
//
// The original Phase G targeted the SHM allocator (removed 2026-05). Reframed
// for the FE/BE-split architecture: where test_qa_fault/test_qa_race pin a
// handful of named cases, these HAMMER the same logic with degenerate caps, the
// exact reset boundary, full recover-then-recur cycles, a deterministic
// equivalence fuzz against a reference model, and high-volume safe-state
// emission — the conditions a long production run actually produces. All pure +
// deterministic (a fixed-seed LCG, no wall-clock, no spawn) so it stays a fast,
// non-flaky ctest. The end-to-end soak/churn lives in examples/qa_soak +
// examples/qa_recover.
//
#include <cstdint>
#include <cstdio>
#include <string>

#include <xi/xi_safe_state.hpp>
#include <xi/xi_respawn_policy.hpp>

// LoggingSafeStateSink always echoes to stderr (by design — a safety transition
// must be visible). The high-volume emission units (ST-U5/U6) call it tens of
// thousands of times, which would bury the ctest log. Silence stderr just around
// those loops via fd redirection; CHECK still records failures in g_failures and
// the post-restore summary reports them. Portable dup/dup2 shim.
#ifdef _WIN32
  #include <io.h>
  #define XI_DUP _dup
  #define XI_DUP2 _dup2
  #define XI_FILENO _fileno
  #define XI_CLOSE _close
  #define XI_NULLDEV "NUL"
#else
  #include <unistd.h>
  #define XI_DUP dup
  #define XI_DUP2 dup2
  #define XI_FILENO fileno
  #define XI_CLOSE close
  #define XI_NULLDEV "/dev/null"
#endif

struct StderrSilencer {
    int saved = -1;
    std::FILE* devnull = nullptr;
    StderrSilencer() {
        std::fflush(stderr);
        saved = XI_DUP(XI_FILENO(stderr));
        devnull = std::fopen(XI_NULLDEV, "w");
        if (devnull) XI_DUP2(XI_FILENO(devnull), XI_FILENO(stderr));
    }
    ~StderrSilencer() {
        std::fflush(stderr);
        if (saved >= 0) { XI_DUP2(saved, XI_FILENO(stderr)); XI_CLOSE(saved); }
        if (devnull) std::fclose(devnull);
    }
};

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Deterministic PRNG (Numerical Recipes LCG). Seeded by a constant so a failure
// always reproduces — never seed from the clock in a test.
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
    int in(int lo, int hi) { return lo + (int)(next() % (uint32_t)(hi - lo + 1)); }
};

// Render one enter_safe_state() call to a string (mirrors the sink's tmpfile use
// in test_qa_race RACE-U2) so we can assert on the emitted line.
static std::string render(const xi::SafeStateEvent& ev) {
    std::string out;
    std::FILE* f = std::tmpfile();
    xi::LoggingSafeStateSink sink(f);
    sink.enter_safe_state(ev);
    std::fflush(f); std::rewind(f);
    char buf[2048]; size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

int main() {
    using xi::SafeStateReason;
    using xi::SafeStateEvent;
    using xi::RespawnTracker;

    // ST-U1: degenerate caps. max=0 must latch safe on the FIRST death (never
    // respawn); max=1 allows exactly one respawn then trips. An off-by-one here
    // would either thrash forever (max=0) or over-respawn.
    {
        RespawnTracker r0;
        CHECK(r0.note_death(0) == true);          // first death already exceeds 0

        RespawnTracker r1;
        CHECK(r1.note_death(1) == false);         // 1st allowed
        CHECK(r1.note_death(1) == true);          // 2nd trips
    }

    // ST-U2: the reset boundary is exact. healthy_for_ms == reset_ms RESETS;
    // one millisecond short does NOT. This is the line between "recovered" and
    // "still in a crash-loop", so it must not drift.
    {
        const int64_t RESET = 30'000;
        RespawnTracker r;
        r.note_death(5); r.note_death(5);
        CHECK(r.consecutive == 2);
        r.note_healthy(RESET - 1, RESET);         // just under -> no reset
        CHECK(r.consecutive == 2);
        r.note_healthy(RESET, RESET);             // exactly at -> reset
        CHECK(r.consecutive == 0);
    }

    // ST-U3: a full recover-then-recur cycle. Climb to one below the cap, get a
    // genuine sustained-healthy recovery (counter forgets), then a fresh crash
    // storm must get its OWN full budget before latching — a recovered line is
    // not penalised for old faults.
    {
        const int CAP = 5;
        const int64_t RESET = 30'000;
        RespawnTracker r;
        for (int i = 0; i < CAP; ++i) CHECK(r.note_death(CAP) == false);  // 5 allowed
        r.note_healthy(RESET, RESET);             // recovered
        CHECK(r.consecutive == 0);
        // Fresh budget: CAP more allowed, then trip on CAP+1.
        int allowed = 0;
        for (int i = 0; i < CAP + 1; ++i) if (!r.note_death(CAP)) ++allowed;
        CHECK(allowed == CAP);
    }

    // ST-U4: equivalence fuzz. 200k interleaved healthy/death steps against a
    // hand-rolled reference model. Invariants checked every step: note_death
    // returns true IFF consecutive would exceed max, and the tracker's counter
    // matches the model exactly. Several seeds + caps so no single schedule hides
    // a bug.
    {
        const int64_t RESET = 30'000;
        for (uint32_t seed : {1u, 7u, 4242u, 0xC0FFEEu}) {
            for (int cap : {0, 1, 3, 5, 16}) {
                Lcg rng(seed * 2654435761u + (uint32_t)cap);
                RespawnTracker r;
                int model = 0;
                for (int step = 0; step < 200'000; ++step) {
                    if (rng.in(0, 2) == 0) {
                        // healthy spell of a random length around the reset point
                        int64_t h = rng.in(0, (int)(RESET * 2));
                        r.note_healthy(h, RESET);
                        if (h >= RESET) model = 0;
                        CHECK(r.consecutive == model);
                    } else {
                        bool tripped = r.note_death(cap);
                        ++model;
                        CHECK(tripped == (model > cap));
                        CHECK(r.consecutive == model);
                    }
                }
            }
        }
    }

    // ST-U5: forensics survive across MANY cap events. fe_main.cpp rebuilds a
    // `stuck` RespawnLimitExceeded event from the triggering BackendExit each
    // time the cap trips; over a long run that happens repeatedly. Assert every
    // emitted cap line still names the faulting module + report an operator must
    // act on (none silently drops to the "-" placeholder).
    {
        StderrSilencer hush;   // each render() echoes to stderr; mute the flood
        for (int i = 0; i < 5000; ++i) {
            SafeStateEvent ev;
            ev.reason          = SafeStateReason::BackendExit;
            ev.faulting_module = "raw_thread_crash.dll";
            ev.report_path     = "C:\\tmp\\xinsp-backend-" + std::to_string(i) + ".json";

            SafeStateEvent stuck;
            stuck.reason          = SafeStateReason::RespawnLimitExceeded;
            stuck.faulting_module = ev.faulting_module;
            stuck.report_path     = ev.report_path;

            std::string s = render(stuck);
            CHECK(s.find("reason=RespawnLimitExceeded") != std::string::npos);
            CHECK(s.find("module=raw_thread_crash.dll") != std::string::npos);
            CHECK(s.find("module=-") == std::string::npos);
        }
    }

    // ST-U6: high-volume emission stays bounded + well-formed. A crash storm can
    // drive thousands of transitions; emitting must not corrupt the line or
    // overrun the fixed 512-byte format buffer even with oversized, attacker-
    // shaped fields. Assert every line is the bounded, prefixed form.
    {
        StderrSilencer hush;
        for (int i = 0; i < 10'000; ++i) {
            SafeStateEvent ev;
            ev.reason          = (i & 1) ? SafeStateReason::PortUnresponsive
                                         : SafeStateReason::CommsLost;
            ev.faulting_module = std::string((size_t)(i % 4096), 'M');  // grows huge
            ev.last_phase      = "inspect\nwith\nnewlines";             // shape attempt
            std::string s = render(ev);
            CHECK(s.rfind("[xinsp-fe] ENTER SAFE STATE", 0) == 0);      // correct prefix
            CHECK(s.size() < 700);                                      // 512 buf + prefix
        }
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(s) FAILED\n", g_failures);
    return 1;
}
