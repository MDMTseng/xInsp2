//
// inspect.cpp — qa_result_order (completion variant). Identical to the arrival
// variant; only the project's parallelism.result_order differs. See
// ../arrival/inspect.cpp for the full explanation of the expose-sink output path.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <chrono>
#include <thread>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    int ms = (frame % 5 == 0) ? 45 : 4;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));

    xi::Record rec;
    rec.set("seq", frame);
    rec.set("$channel", "order");
    xi::use("expose").process(rec);
}
