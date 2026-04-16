#include <xi/xi.hpp>
XI_SCRIPT_EXPORT void xi_inspect_entry(int frame) {
    VAR(before, std::string("about to crash"));
    int* p = nullptr;
    *p = 42;
    VAR(never, std::string("should never reach here"));
}
