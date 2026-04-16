#include <xi/xi.hpp>
XI_SCRIPT_EXPORT void xi_inspect_entry(int frame) {
    VAR(before, std::string("about to divide by zero"));
    int a = 10, b = 0;
    int c = a / b;
    VAR(never, c);
}
