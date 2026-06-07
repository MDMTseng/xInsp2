#include <xi/xi.hpp>
#include <xi/xi_result.hpp>
#include <thread>
#include <chrono>
XI_SCRIPT_EXPORT void xi_inspect_entry(int){
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // back up the queue
    xi::ok(1, "slow");
}
