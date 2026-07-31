#pragma once
//
// xi_result_class.hpp — the ONE home for the run-result reserved band.
//
// Shared by the live backend (service_state.hpp / service_result.cpp) and
// the headless runner (runner_main.cpp), which used to carry an acknowledged
// hand-mirrored copy of these constants + the class mapping + the reserved-band
// rejection ("mirrors service_main::outcome_class_for_code"). One header, no
// drift. Lives in backend/src (not the SDK include dir): both consumers are
// backend binaries; the script-facing side of the band is xi_result.hpp.
//
// See docs/roadmap/run-result.md for the band design.

#include <string>

// Framework system-fail enum: a reserved band (<= kResultSystemBand) the user
// API refuses to set; XI_SYS_* are the named markers the host itself emits.
enum : int {
    XI_SYS_DROPPED    = -999001,  // overflow: event dropped before it could run
    XI_SYS_CRASHED    = -999002,  // caught inspect error (throw/crash) — the run did not verdict
    XI_SYS_NO_VERDICT = -999005,  // ran to completion but script set no RESULT (was v1.1 opt-in)
};
static constexpr int kResultSystemBand = -990000;

// Stable schema tag for the run_result wire event / headless report (bump on a
// breaking change to the field set). Rides as an additive "schema" field.
static constexpr const char* kRunResultSchema = "xi.run-outcome/1";

// Derive the outcome class string from the EXISTING signed code — a pure read,
// it never changes the numeric code. Bands: >0 → "ok"; ==0 → "na"; the reserved
// system markers map to their own classes (dropped/crashed/no_verdict); a valid
// ng code (<0 and above the reserved system band) → "ng"; anything else in the
// system band → "na". The crash/no-verdict paths emit their own reserved codes,
// so class and code agree even when the emitter derives the class.
inline const char* outcome_class_for_code(int code) {
    if (code == XI_SYS_DROPPED)     return "dropped";
    if (code == XI_SYS_CRASHED)     return "crashed";
    if (code == XI_SYS_NO_VERDICT)  return "no_verdict";
    if (code > 0)                 return "ok";
    if (code == 0)                return "na";
    if (code > kResultSystemBand) return "ng";   // valid ng band: <0 and > -990000
    return "na";                                  // other reserved system codes
}

// result_cb reserved-band rejection core. The host is the trust boundary: a
// user code in the reserved system band is NOT accepted as a real verdict —
// it is recorded as NA (0) with the offending code preserved in the message.
// Returns true when the code was rejected (out_code/out_msg filled); the
// caller adds its own observability (WS warn log in the backend, nothing in
// the runner) and stores the pair either way.
inline bool reject_reserved_result_code(int code, const char* msg,
                                        int& out_code, std::string& out_msg) {
    if (code > kResultSystemBand) return false;
    out_code = 0;   // NA, not a fake ng
    out_msg  = "[invalid result code " + std::to_string(code) + ", reserved band] ";
    out_msg += (msg ? msg : "");
    return true;
}
