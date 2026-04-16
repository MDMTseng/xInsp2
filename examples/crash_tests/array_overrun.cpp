#include <xi/xi.hpp>
#include <vector>
XI_SCRIPT_EXPORT void xi_inspect_entry(int frame) {
    VAR(before, std::string("about to overrun"));
    int arr[4] = {1,2,3,4};
    // Write way past the end — triggers access violation
    volatile int* p = arr;
    p[100000] = 99;
    VAR(never, std::string("should not reach"));
}
