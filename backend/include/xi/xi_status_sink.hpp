#pragma once
//
// xi_status_sink.hpp — indirection so a plugin's host_api->set_status() can
// reach the backend's status registry without xi_core depending on the backend.
//
// Same pattern as emit_trigger → TriggerBus: make_host_api() (in xi_core) wires
// api.set_status to call through this sink; the backend (service_main) installs
// the actual sink at startup. If no sink is installed (e.g. headless runner),
// set_status is a harmless no-op.
//
namespace xi {

// (source, text): source is the instance name (or "@script"); empty/null source
// is attributed by the host (it uses "@plugin").
using StatusSinkFn = void (*)(const char* source, const char* text);

inline StatusSinkFn& status_sink() {
    static StatusSinkFn fn = nullptr;
    return fn;
}

} // namespace xi
