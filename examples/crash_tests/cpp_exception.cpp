#include <xi/xi.hpp>
#include <stdexcept>
XI_SCRIPT_EXPORT void xi_inspect_entry(int frame) {
    VAR(before, std::string("about to throw"));
    throw std::runtime_error("user script intentional error");
    VAR(never, std::string("should not reach"));
}
