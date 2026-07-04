// qa_overflow_block — a deliberately SLOW inspect (~40ms) so a 12ms-paced source
// out-runs the drain. On the "blk" lane (overflow:block) the source back-pressures
// (parks) instead of dropping → lossless; on the "drp" lane (overflow:drop_oldest)
// the surplus is dropped. Trivial compute; the sleep IS the drain rate.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>
#include <chrono>
#include <thread>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto t = xi::current_trigger();
    if (!t.is_active()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    xi::ok(1, t.primary_source());
}
