// qa_group_parallelism — each inspect sleeps a random 50-100ms so concurrent
// work overlaps. Three groups (p1/p2/p4) get a 1Hz burst of 3x their worker
// count, so each group's queue fills and all its max_parallel lanes engage at
// once. The driver polls dispatch_stats `running` to confirm each group hits
// (and never exceeds) its max_parallel.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>
#include <chrono>
#include <random>
#include <thread>

XI_INSPECT_ENTRY(t, /*frame*/ frame) {
    (void)frame;
    if (!t.is_active()) return;   // skip the synthetic timer tick

    const std::string src = t.primary_source();
    // Surface which source fired through `expose` (channel "grp"), pack-only:
    // expose is a sink, so a script-built pack is pushed (staged + flushed in
    // the emit gate) — the pack equivalent of the old process(Record) leg.
    xi::ScriptPackBuilder eb;
    eb.add_str("$channel", "grp");
    eb.add_str("src", src);
    xi::use("expose").push(eb.seal());
    // Random 50-100ms of work, per-thread RNG so parallel lanes don't contend.
    static thread_local std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> d(50, 100);
    std::this_thread::sleep_for(std::chrono::milliseconds(d(rng)));
    xi::ok(1, src);
}
