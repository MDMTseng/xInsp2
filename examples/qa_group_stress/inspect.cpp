// qa_group_stress — 8 groups x max_parallel 4, each fed a 20/s burst. Each inspect
// sleeps a random 150-220ms (avg ~185ms). With 4 workers that's a ~21.6/s per-group
// capacity vs 20/s arrival — near-saturation, so the queue drains each second on
// AVERAGE without piling up. The driver polls dispatch_stats `running` to confirm
// each group still hits (and never exceeds) max_parallel, with no drops.
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
    // Surface which source fired through `expose` (channel "grp").
    xi::ScriptPackBuilder b;
    b.add_str("$channel", "grp");
    b.add_str("src", src);
    xi::use("expose").push(b.seal());
    // Per-thread RNG so the parallel lanes don't contend on it.
    static thread_local std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> d(150, 220);
    std::this_thread::sleep_for(std::chrono::milliseconds(d(rng)));
    xi::ok(1, src);
}
