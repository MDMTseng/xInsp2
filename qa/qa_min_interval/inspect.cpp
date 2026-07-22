// qa_min_interval — both groups get a STEADY 20/s; "limited" has min_interval_ms:100
// (rate cap ~10/s, surplus coalesced via queue_depth:5 drop_oldest), "free" is
// uncapped. Trivial inspect (5ms) so the rate cap, not compute, limits "limited".
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>
#include <chrono>
#include <thread>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    if (!t.is_active()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    xi::ok(1, t.primary_source());
}
