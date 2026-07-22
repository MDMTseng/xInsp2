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
#include "xi_abi.h"   // xi_bin_span (the zero-copy owned-emit segment type)

namespace xi {

using BinarySinkFn = void (*)(const void* data, int len);

// The ZERO-COPY owned-emit sink (xi.emit@2 / perf/ws-lean). Same boot-once-install
// discipline + rationale as binary_sink() above: the backend installs it once at
// startup (service_main) forwarding to the WS server's send_binary_owned; headless
// leaves it null. `owner`+`release` transfer ownership of the segment bytes to the
// host, which releases (in the producer's TU) exactly once after the send/drop/
// teardown. A caller that reaches this with NO sink installed MUST release the
// owner itself (the xi_core forwarder does — see get_interface_impl).
using BinaryOwnedSinkFn = void (*)(const xi_bin_span* spans, int nspans,
                                   void* owner, void (*release)(void*));
inline BinaryOwnedSinkFn& binary_owned_sink() {
    static BinaryOwnedSinkFn fn = nullptr;
    return fn;
}

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
