// burst_dispatch — measure how dispatch_threads parallelises slow inspects.
//
// Each call to xi_inspect_entry asks `det.process()` to sleep ~50 ms inside
// the worker. With dispatch_threads=4, four calls should overlap and 8
// frames take ~100 ms wall-clock instead of ~400 ms.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto& det = xi::use("det");
    auto out  = det.process(xi::Record().set("sleep_ms", 50));
    VAR(count, out["count"].as_int(0));
}
