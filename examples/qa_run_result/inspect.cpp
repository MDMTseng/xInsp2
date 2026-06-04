// Per-run Result smoke test (Phase 1). Cycles the verdict every inspect so the
// driver can assert the run_result event's code: NA (0, no xi::result call),
// then ok1 (+1), then ng2 (-2). The 4th tries to set a reserved system code —
// the host rejects it (records NA + warns + names the bad code), so user scripts
// can't squat the framework band and the mistake stays visible.
#include <xi/xi.hpp>
#include <xi/xi_result.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    static int n = 0;
    switch (n++ % 4) {
        case 0: /* set nothing → run_result defaults to 0 (NA) */            break;
        case 1: xi::ok(1, "clean");                                          break;
        case 2: xi::ng(2, "edge chip > 0.3mm");                              break;
        case 3: xi::result(-999001, "trying to squat XI_SYS_DROPPED");       break;  // -> NA + warning
    }
}
