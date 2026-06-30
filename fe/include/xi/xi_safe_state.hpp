#pragma once
//
// xi_safe_state.hpp — the FE supervisor's crash-event model.
//
// Process isolation was removed (2026-05): a hard plugin crash takes the whole
// backend (BE) down. The FE (xinsp-fe.exe) supervises: it detects the BE dying,
// classifies WHY, records it to the crash history, and respawns.
//
// The "drive the production line to a safe state on BE crash" action (the PLC
// SafeStateSink + the host->set_safe_state hook) was REMOVED 2026-06: that
// belongs to a comms plugin, which spins off its OWN sidecar process that talks
// to the PLC and — watching the BE's process handle — sends the line-safe message
// itself when the BE dies. The core/FE no longer owns it.
//
// What remains here is just the crash EVENT (reason + forensic context) the FE
// records. Deliberately portable: only std headers, no Win32 / no OpenCV (Linux).
//
#include <cstdint>
#include <string>

namespace xi {

// Why the FE observed the backend go down.
enum class SafeStateReason {
    BackendExit,            // BE process exited (crash or unexpected exit)
    PortUnresponsive,       // BE process alive but WS port stopped accepting
    BootTimeout,            // BE alive + port bound but never reached "ready" in time
    CommsLost,              // INERT/reserved — nothing emits this today.
    RespawnLimitExceeded,   // too many crashes in the window; giving up
    SupervisorShutdown,     // the FE itself is shutting down (intended)
};

inline const char* to_string(SafeStateReason r) {
    switch (r) {
        case SafeStateReason::BackendExit:          return "BackendExit";
        case SafeStateReason::PortUnresponsive:     return "PortUnresponsive";
        case SafeStateReason::BootTimeout:          return "BootTimeout";
        case SafeStateReason::CommsLost:            return "CommsLost";
        case SafeStateReason::RespawnLimitExceeded: return "RespawnLimitExceeded";
        case SafeStateReason::SupervisorShutdown:   return "SupervisorShutdown";
    }
    return "Unknown";
}

// Forensic context the FE records when the BE goes down (crash history / status /
// operator HMI). All fields are best-effort — a clean SupervisorShutdown carries
// no crash data.
struct SafeStateEvent {
    SafeStateReason reason       = SafeStateReason::BackendExit;
    int             backend_rc   = 0;   // BE process exit code (e.g. 0xC0000005)
    std::string     exception_name;     // e.g. "ACCESS_VIOLATION"
    std::string     faulting_module;    // e.g. "plugin_v0.dll"
    std::string     last_phase;         // dispatch-thread breadcrumb, e.g. "inspect"
    std::string     report_path;        // path to the .json crash report, if any
    std::string     dump_path;          // path to the .dmp minidump, if any
    int64_t         ts_ms        = 0;   // wall-clock ms when the FE observed the death
    // Part III G2.1 — per-plugin crash attribution. The backend stamps a
    // process-global "current culprit" (the plugin/instance it most recently
    // entered) into the crash report; enrich_from_crash_report joins it to the
    // faulting module so the FE knows WHICH plugin caused the death and can
    // quarantine it (vs. only "the backend died"). Empty when the fault wasn't
    // attributable to a plugin (e.g. a crash in the script or the core itself).
    std::string     culprit_plugin;     // attributed plugin id (name), if any
    std::string     culprit_instance;   // instance being serviced at the fault
    std::string     culprit_folder;     // that plugin's folder (so the FE can
                                        //   quarantine it via G1's .xi_certify.json)
    std::string     culprit_dll;        // dll filename within that folder
};

} // namespace xi
