#pragma once
//
// xi_safe_state.hpp — the FE supervisor's "tell the line to go safe" seam.
//
// Process isolation was removed (2026-05): a hard plugin crash takes the whole
// backend (BE) compute core down. The frontend supervisor (xinsp-fe.exe) is the
// replacement safety net — the instant it sees the BE die (or become
// unresponsive), it drives the line into a safe state via a SafeStateSink, then
// resilizes the BE.
//
// SafeStateSink is the abstraction over "whatever commands the line to a safe
// state" — a PLC coil over Modbus, an OPC-UA node, a digital-out line, etc. None
// of those exist yet; the only concrete impl today is LoggingSafeStateSink, a
// stub that records the transition. Real transports slot into make_safe_state_sink().
//
// Deliberately portable: only <string>/<memory>/<cstdint>/<cstdio>, no Win32 and
// no OpenCV, so it compiles unchanged on Linux (see docs/design/linux-port.md).
//
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace xi {

// Why the FE is asking the line to go (or leave) safe.
enum class SafeStateReason {
    BackendExit,            // BE process exited (crash or unexpected exit)
    PortUnresponsive,       // BE process alive but WS port stopped accepting
    BootTimeout,            // BE alive + port bound but never reached "ready" in time
    RespawnLimitExceeded,   // too many crashes in the window; giving up, staying safe
    SupervisorShutdown,     // the FE itself is shutting down (intended)
};

inline const char* to_string(SafeStateReason r) {
    switch (r) {
        case SafeStateReason::BackendExit:          return "BackendExit";
        case SafeStateReason::PortUnresponsive:     return "PortUnresponsive";
        case SafeStateReason::BootTimeout:          return "BootTimeout";
        case SafeStateReason::RespawnLimitExceeded: return "RespawnLimitExceeded";
        case SafeStateReason::SupervisorShutdown:   return "SupervisorShutdown";
    }
    return "Unknown";
}

// Forensic context handed to the sink so a real PLC integration (or an operator
// HMI reading the FE log) can record WHY the line went safe. All fields are
// best-effort — a clean SupervisorShutdown carries no crash data.
struct SafeStateEvent {
    SafeStateReason reason       = SafeStateReason::BackendExit;
    int             backend_rc   = 0;   // BE process exit code (e.g. 0xC0000005)
    std::string     exception_name;     // from the crash report, e.g. "ACCESS_VIOLATION"
    std::string     faulting_module;    // e.g. "plugin_v0.dll"
    std::string     last_phase;         // dispatch-thread breadcrumb, e.g. "inspect"
    std::string     report_path;        // path to the .json crash report, if any
    int64_t         ts_ms        = 0;   // wall-clock ms when the FE decided to go safe
};

// The seam. enter_safe_state() must be safe to call from the FE's monitor thread
// and must not throw (the FE treats a safe-state command as the last thing it can
// do for the line — swallow/log transport errors internally).
class SafeStateSink {
public:
    virtual ~SafeStateSink() = default;
    virtual void enter_safe_state(const SafeStateEvent& ev) = 0;
    virtual void clear_safe_state() = 0;
    virtual const char* name() const = 0;
};

// Stub sink: records the transition to a log stream + stderr. This is the only
// concrete impl today — it makes the FE's safe-state decisions observable (and
// testable) without any hardware. A real PLC transport replaces it via the
// factory below.
class LoggingSafeStateSink : public SafeStateSink {
public:
    // log may be null -> stderr only. The sink does not own the FILE*.
    explicit LoggingSafeStateSink(std::FILE* log = nullptr) : log_(log) {}

    void enter_safe_state(const SafeStateEvent& ev) override {
        // ts= (wall-clock epoch-ms the FE decided to go safe) goes at the END so
        // the "ENTER SAFE STATE reason=..." prefix stays stable for log scrapers;
        // a safety transition log needs a timestamp for post-incident forensics.
        emit("ENTER SAFE STATE reason=%s rc=0x%08X module=%s phase=%s report=%s ts=%lld",
             to_string(ev.reason),
             static_cast<unsigned>(ev.backend_rc),
             ev.faulting_module.empty() ? "-" : ev.faulting_module.c_str(),
             ev.last_phase.empty()      ? "-" : ev.last_phase.c_str(),
             ev.report_path.empty()     ? "-" : ev.report_path.c_str(),
             static_cast<long long>(ev.ts_ms));
    }

    void clear_safe_state() override {
        emit("CLEAR SAFE STATE");
    }

    const char* name() const override { return "log"; }

private:
    void emit(const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        std::fprintf(stderr, "[xinsp-fe] %s\n", buf);
        std::fflush(stderr);
        if (log_) {
            std::fprintf(log_, "[xinsp-fe] %s\n", buf);
            std::fflush(log_);
        }
    }

    std::FILE* log_;
};

// Factory: pick a sink by type string ("log"/empty -> logging). Future PLC
// transports ("modbus", "opcua", "digital_out") slot in here behind their own
// #ifdef/availability guards, keeping the rest of the FE oblivious to the wire.
inline std::unique_ptr<SafeStateSink> make_safe_state_sink(const std::string& type,
                                                           std::FILE* log = nullptr) {
    (void)type;  // only "log" is supported today; everything falls through to it
    return std::make_unique<LoggingSafeStateSink>(log);
}

} // namespace xi
