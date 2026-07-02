#include <xi/xi.hpp>
#include <xi/xi_status.hpp>
#include <vector>
XI_SCRIPT_EXPORT void xi_inspect_entry(int frame) {
    xi::status("about to crash: array overrun");
    int arr[4] = {1,2,3,4};
    // Write way past the end — triggers access violation
    volatile int* p = arr;
    p[100000] = 99;
}
