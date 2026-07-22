#include <xi/xi.hpp>
#include <xi/xi_status.hpp>
#include <stdexcept>
XI_SCRIPT_EXPORT void xi_inspect_entry(int frame) {
    xi::status("about to crash: throw std::runtime_error");
    throw std::runtime_error("user script intentional error");
}
