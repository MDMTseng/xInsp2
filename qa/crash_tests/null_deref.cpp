#include <xi/xi.hpp>
#include <xi/xi_status.hpp>
XI_SCRIPT_EXPORT void xi_inspect_entry(int frame) {
    xi::status("about to crash: null deref");
    int* p = nullptr;
    *p = 42;
}
