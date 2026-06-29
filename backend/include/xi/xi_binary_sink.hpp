#pragma once
//
// xi_binary_sink.hpp — indirection so a plugin's host_api->emit_binary() can
// reach the backend's WS server without xi_core depending on the backend.
//
// Same pattern as xi_status_sink.hpp: make_host_api() (in xi_core) wires
// api.emit_binary to call through this sink; the backend (service_main) installs
// the actual sink at startup (forwarding to the WS server's binary send). With no
// sink installed (e.g. the headless runner) emit_binary is a harmless no-op.
//
namespace xi {

using BinarySinkFn = void (*)(const void* data, int len);

inline BinarySinkFn& binary_sink() {
    static BinarySinkFn fn = nullptr;
    return fn;
}

} // namespace xi
