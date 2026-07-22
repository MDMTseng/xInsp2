// lifecycle_teardown fixture — a trivial inspect driven by two free-running
// burst_source instances. The small sleep keeps an inspect IN-FLIGHT while the
// driver issues close_project / shutdown, which is exactly the window the #6
// UAF regression guards (a one-shot inspect launching on the source's emit
// thread into freed code after FreeLibrary).
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
    xi::ok(1, "ok");
}
