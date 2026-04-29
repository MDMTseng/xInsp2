// inspect.cpp — config_validation_demo.
//
// Trivial inspect: this demo's entire point is exercising the
// open-time validation of instance.json.config against
// plugin.json.manifest.params. We don't actually run frames;
// driver.py opens the project and reads the warnings list.
// A non-empty inspect body is still required by the framework so
// compile_and_load can succeed if anyone wants to push frames.

#include <xi/xi.hpp>

void xi_inspect_entry() {
    // no-op
}
