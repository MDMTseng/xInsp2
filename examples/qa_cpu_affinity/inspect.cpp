// qa_cpu_affinity — each inspect records the CPU core it actually ran on
// (GetCurrentProcessorNumber). The "bound" group's workers have cpu_affinity
// [2,3,4,5] (a MULTI-core mask — each worker may run on any of those four, not
// pinned to one); the "free" group is unbound. The driver checks the bound
// group's runs only ever land on cores 2-5, while free spreads anywhere.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>
#include <chrono>
#include <thread>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    if (!t.is_active()) return;

    // A little work so the thread is actually scheduled on a core for a while.
    // 40 ms (not a token few) widens the window in which the group's two lanes
    // are concurrently in-flight, so the scheduler reliably places them on two
    // different masked cores — the multi-core-spread signal the driver checks.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    int core = -1;
#ifdef _WIN32
    core = (int)GetCurrentProcessorNumber();
#endif
    // Result message = the core this run executed on (the driver parses it).
    xi::ok(1, std::to_string(core));
}
