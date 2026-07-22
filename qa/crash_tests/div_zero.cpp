#include <xi/xi.hpp>
#include <xi/xi_status.hpp>
XI_SCRIPT_EXPORT void xi_inspect_entry(int frame) {
    xi::status("about to crash: divide by zero");
    int a = 10, b = 0;
    int c = a / b;
    (void)c;
}
