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

// The holder is a PLAIN non-atomic static. This is safe ONLY under the boot-once-
// install discipline: the backend installs the sink once at startup (service_main)
// before any plugin/worker thread can call through it, and never re-installs it at
// runtime. A runtime re-install would be a data race against concurrent callers.
// The thread-safety send_binary relies on (asserted in service_main) comes from the
// WS-server serialization the installed fn forwards to, NOT from this holder.
inline BinarySinkFn& binary_sink() {
    static BinarySinkFn fn = nullptr;
    return fn;
}

} // namespace xi
