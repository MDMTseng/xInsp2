// qa_plugin_queue_sim — a deliberately SLOW inspect (~40ms) so the source's emit
// is paced entirely by the core lane's back-pressure. On the "rv" lane
// (queue_depth:0, overflow:block) each emit RENDEZVOUS-blocks until a worker takes
// the frame; on the "pipe" lane (queue_depth:1) the producer runs one ahead. Both
// are lossless — the sleep IS the drain rate, and the plugin's own deque (not the
// core lane) holds the backlog. Trivial compute.
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
