#pragma once
//
// xi_log_sink.hpp — indirection so a plugin/script host_api->log() can reach the
// backend's operator-visible log channel (WS log + recent-errors ring) without
// xi_core depending on the backend.
//
// Same pattern as xi_status_sink / xi_binary_sink: make_host_api() (in xi_core)
// always prints the line to stderr, then ALSO calls through this sink; the backend
// (service_main) installs the actual sink at startup and forwards WARN/ERROR to the
// WS log so an unattended PC (where stderr is unwatched) still surfaces a plugin's
// self-diagnostics. If no sink is installed (e.g. headless runner) it's a no-op —
// stderr still has the line.
//
#include <cstdint>

namespace xi {

// (level, msg, ts_ms): level matches host_api->log (0=DEBUG 1=INFO 2=WARN 3=ERROR);
// ts_ms is wall-clock epoch millis (stamped by make_host_api) so the operator
// channel and a crash sidecar agree on time.
using LogSinkFn = void (*)(int32_t level, const char* msg, int64_t ts_ms);

inline LogSinkFn& log_sink() {
    static LogSinkFn fn = nullptr;
    return fn;
}

} // namespace xi
