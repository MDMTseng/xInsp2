//
// test_reactive.cpp — the demand-gate / dedup / stats contract of
// xi::Derived<Out> (xi_reactive.hpp). Pure logic, no host: demand, project and
// sink are fakes, so the four outcomes (Suspended / Deduped / Derived / Failed)
// and the ordering law (gate before dedup before project) are checked exactly.
//
#include <xi/xi_reactive.hpp>
#include <xi/xi_test.hpp>

#include <string>

using xi::Demand;
using xi::Derived;
using S = xi::Derived<std::string>::Status;

namespace {

// A rig with knobs for demand + a failing projection, and counters proving what
// actually ran (the whole point: gated work must NOT run).
struct Rig {
    int      viewers      = 1;
    uint64_t window       = 0;   // Demand::window — folds into the dedup key
    bool     project_ok   = true;
    int      project_runs = 0;
    int      sink_runs    = 0;
    std::string last_sunk;

    Derived<std::string> make() {
        return Derived<std::string>(
            "rig",
            [this] { return Demand{ viewers, window }; },
            [this](std::string& out) {
                ++project_runs;
                if (!project_ok) return false;
                out = "frame#" + std::to_string(project_runs);
                return true;
            },
            [this](const std::string& f) { ++sink_runs; last_sunk = f; });
    }
};

} // namespace

// No viewer => Suspended: project and sink must never run, no value retained.
XI_TEST(gate_suspends_when_unwatched) {
    Rig rig; rig.viewers = 0;
    auto cell = rig.make();

    auto r = cell.refresh(0xAAAA);
    XI_EXPECT(r.status == S::Suspended);
    XI_EXPECT(r.value == nullptr);
    XI_EXPECT(!r.served());
    XI_EXPECT_EQ(rig.project_runs, 0);   // gate is BEFORE project
    XI_EXPECT_EQ(rig.sink_runs, 0);
    XI_EXPECT(!cell.primed());
    XI_EXPECT_EQ(cell.stats().suspended, (uint64_t)1);
}

// First watched frame => Derived: project + sink run once, value handed back.
XI_TEST(first_watched_frame_derives_and_sinks) {
    Rig rig;
    auto cell = rig.make();

    auto r = cell.refresh(0x1111);
    XI_EXPECT(r.status == S::Derived);
    XI_EXPECT(r.changed());
    XI_EXPECT(r.value != nullptr);
    XI_EXPECT_EQ(*r.value, std::string("frame#1"));
    XI_EXPECT_EQ(rig.project_runs, 1);
    XI_EXPECT_EQ(rig.sink_runs, 1);
    XI_EXPECT_EQ(rig.last_sunk, std::string("frame#1"));
}

// Same input hash => Deduped: project + sink do NOT run again; the retained
// value is handed back unchanged.
XI_TEST(unchanged_input_dedups_no_reproject) {
    Rig rig;
    auto cell = rig.make();

    cell.refresh(0x2222);                 // Derived
    auto r = cell.refresh(0x2222);        // same hash
    XI_EXPECT(r.status == S::Deduped);
    XI_EXPECT(r.served());
    XI_EXPECT(!r.changed());
    XI_EXPECT(r.value != nullptr);
    XI_EXPECT_EQ(*r.value, std::string("frame#1"));
    XI_EXPECT_EQ(rig.project_runs, 1);    // dedup is BEFORE project
    XI_EXPECT_EQ(rig.sink_runs, 1);
}

// A new input hash after a dedup re-projects.
XI_TEST(changed_input_reprojects) {
    Rig rig;
    auto cell = rig.make();

    cell.refresh(0x3333);                 // Derived frame#1
    cell.refresh(0x3333);                 // Deduped
    auto r = cell.refresh(0x4444);        // changed => Derived frame#2
    XI_EXPECT(r.status == S::Derived);
    XI_EXPECT_EQ(*r.value, std::string("frame#2"));
    XI_EXPECT_EQ(rig.project_runs, 2);
    XI_EXPECT_EQ(rig.sink_runs, 2);
}

// Demand can drop back to zero between ticks; the gate shuts again and the
// retained value is NOT re-served (Suspended, not Deduped).
XI_TEST(demand_drop_reshuts_gate) {
    Rig rig;
    auto cell = rig.make();

    cell.refresh(0x5555);                 // watched => Derived
    rig.viewers = 0;
    auto r = cell.refresh(0x5555);        // same hash but now unwatched
    XI_EXPECT(r.status == S::Suspended);
    XI_EXPECT(r.value == nullptr);
    XI_EXPECT_EQ(rig.project_runs, 1);
    XI_EXPECT_EQ(rig.sink_runs, 1);
}

// A failing projection => Failed: sink does not run, no value retained, and a
// later good tick still works (failure is not sticky).
XI_TEST(failed_projection_reports_and_recovers) {
    Rig rig; rig.project_ok = false;
    auto cell = rig.make();

    auto r = cell.refresh(0x6666);
    XI_EXPECT(r.status == S::Failed);
    XI_EXPECT(r.value == nullptr);
    XI_EXPECT_EQ(rig.sink_runs, 0);
    XI_EXPECT(!cell.primed());

    rig.project_ok = true;
    auto r2 = cell.refresh(0x6666);       // same hash, but nothing was retained
    XI_EXPECT(r2.status == S::Derived);
    XI_EXPECT_EQ(rig.sink_runs, 1);
}

// Demand::window folds into the dedup key: same input pixels but a changed
// viewport re-projects (pan/zoom the same frame => a different crop); an
// unchanged window still dedups. This is what makes window meaningful at the
// core, not a passenger the application hashes in by hand.
XI_TEST(window_change_reprojects_same_input) {
    Rig rig; rig.window = 0x0001'0002'0003'0004ull;
    auto cell = rig.make();

    cell.refresh(0x8888);                 // Derived (window A)
    auto same = cell.refresh(0x8888);     // same input + same window => Deduped
    XI_EXPECT(same.status == S::Deduped);
    XI_EXPECT_EQ(rig.project_runs, 1);

    rig.window = 0x0009'0009'0009'0009ull; // viewport moved
    auto moved = cell.refresh(0x8888);    // SAME input pixels, new window
    XI_EXPECT(moved.status == S::Derived);
    XI_EXPECT_EQ(rig.project_runs, 2);
}

// invalidate() forces a re-project on an otherwise-deduped input (the
// "projection parameters changed" seam, e.g. a new viewport).
XI_TEST(invalidate_forces_reproject) {
    Rig rig;
    auto cell = rig.make();

    cell.refresh(0x7777);                 // Derived
    cell.invalidate();
    auto r = cell.refresh(0x7777);        // same hash, but memo dropped
    XI_EXPECT(r.status == S::Derived);
    XI_EXPECT_EQ(rig.project_runs, 2);
}

// The stats ledger accounts every tick into exactly one bucket.
XI_TEST(stats_account_every_tick) {
    Rig rig;
    auto cell = rig.make();

    rig.viewers = 0; cell.refresh(0x10);              // suspended
    rig.viewers = 1; cell.refresh(0x20);              // derived
    cell.refresh(0x20);                                // deduped
    cell.refresh(0x30);                                // derived
    rig.project_ok = false; cell.refresh(0x40);        // failed
    rig.project_ok = true;

    const auto& s = cell.stats();
    XI_EXPECT_EQ(s.ticks, (uint64_t)5);
    XI_EXPECT_EQ(s.suspended, (uint64_t)1);
    XI_EXPECT_EQ(s.derived, (uint64_t)2);
    XI_EXPECT_EQ(s.deduped, (uint64_t)1);
    XI_EXPECT_EQ(s.failed, (uint64_t)1);
    XI_EXPECT_EQ(s.suspended + s.deduped + s.derived + s.failed, s.ticks);
}

// fnv1a is a stable content identity: same bytes => same hash, one flipped byte
// => different (this is the dedup key UiView feeds refresh()).
XI_TEST(fnv1a_is_stable_content_identity) {
    const uint8_t a[] = {1, 2, 3, 4, 5};
    uint8_t b[] = {1, 2, 3, 4, 5};
    XI_EXPECT_EQ(xi::fnv1a(a, sizeof a), xi::fnv1a(b, sizeof b));
    b[2] = 0x99;
    XI_EXPECT(xi::fnv1a(a, sizeof a) != xi::fnv1a(b, sizeof b));
}

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}
