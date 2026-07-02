// Per-run Result / run-outcome contract test.
//
// Cycles the verdict on every inspect so the driver can assert the run_result
// event's identity/outcome contract (schema "xi.run-outcome/1"). The 6-case
// cycle exercises every outcome class the script can drive from user code:
//
//   case 0  set nothing            -> code -999005 (XI_SYS_NO_VERDICT) class "no_verdict"
//                                     BREAKING vs v1 (which emitted 0/NA): a run that
//                                     completes but sets no result is now distinct.
//   case 1  xi::ok(1,"clean")      -> code +1      class "ok"
//   case 2  xi::ng(2,"edge chip")  -> code -2      class "ng"
//   case 3  xi::result(0,"...")    -> code  0      class "na"  (EXPLICIT NA)
//   case 4  throw std::exception   -> code -999002 (XI_SYS_CRASHED) class "crashed"
//                                     BREAKING vs v1 (which emitted 0/NA): a caught
//                                     inspect error is now distinct on the numeric
//                                     channel. The host's SEH/exception guard catches
//                                     this throw, so the process stays up.
//   case 5  xi::result(-999001,..) -> code  0      class "na"  (reserved-band squat:
//                                     the host refuses the framework system band,
//                                     records NA + warns, names the offending code.)
//
// The dropped (XI_SYS_DROPPED) case is driven separately by the driver's Test 2
// (a slow inspect under a fast timer with queue_depth:1 / drop_oldest); it cannot
// be produced from inside a single inspect body, so it is not part of this cycle.
#include <xi/xi.hpp>
#include <xi/xi_result.hpp>
#include <stdexcept>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    static int n = 0;
    switch (n++ % 6) {
        case 0: /* set nothing -> run_result NO_VERDICT (-999005) */          break;
        case 1: xi::ok(1, "clean");                                           break;
        case 2: xi::ng(2, "edge chip > 0.3mm");                              break;
        case 3: xi::result(0, "explicit NA — not applicable");               break;
        case 4: throw std::runtime_error("deliberate inspect fault");         break;
        case 5: xi::result(-999001, "trying to squat XI_SYS_DROPPED");        break;
    }
}
