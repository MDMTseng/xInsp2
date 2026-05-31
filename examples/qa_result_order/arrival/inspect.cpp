//
// inspect.cpp — qa_result_order. Uneven inspect time (every 5th frame is slow)
// so that under dispatch_threads>1 a later, faster frame finishes before an
// earlier slow one. With result_order:"arrival" the backend must still emit
// vars in frame-arrival order; with "completion" they interleave.
//
#include <xi/xi.hpp>
#include <chrono>
#include <thread>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    int ms = (frame % 5 == 0) ? 45 : 4;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    VAR(seq, frame);
}
