#pragma once
//
// xi_be_health.hpp — the FE's read side of the BE health mirror.
//
// The FE supervisor cannot call `get_health` on the backend: that needs a WS
// client, and the backend accepts exactly ONE — a slot reserved for the operator
// HMI / VS Code extension (review 09; docs/internals/fe-be.md). Stealing it would
// lock the real UI out. So the BE instead mirrors just its top-level health into a
// tiny file (service_health.cpp write_health_mirror_, on health_changed only), and
// the FE polls THAT — the same file-not-socket pattern as the BE heartbeat and the
// FE's own fe-status.json. The FE then re-exposes it in fe_status so a UI reads the
// BE's canonical state (running / degraded / fault …) instead of inferring it.
//
// This header is the PURE parser (no I/O, no Win32) so it can be unit-tested. The
// FE reads the file bytes and hands them here; render into fe_status is in
// xi_fe_status.hpp.
//
// Wire shape (schema-free — it is a private FE↔BE channel, versioned implicitly
// with the pair):  {"state":"running","since_ms":N,"last_reason":"","ts_ms":N}
//
#include <cstdint>
#include <string>

#include <yyjson.h>

namespace xi {

// The BE's mirrored top-level health, as the FE consumes it.
struct BeHealth {
    std::string state;          // boot|project_loaded|running|degraded|draining|fault
    int64_t     since_ms = 0;   // wall-ms the current state was entered
    std::string last_reason;    // plugin_fault|compile_error|watchdog_trip|"" (clean)
    bool        valid = false;  // false → file missing/empty/unparseable/no "state"
};

// Parse the mirror file's contents. Returns false (and leaves out.valid=false) on
// any malformed input — a torn/half-written read just means "no update this poll".
inline bool parse_be_health(const std::string& json, BeHealth& out) {
    out = BeHealth{};
    if (json.empty()) return false;
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (!doc) return false;
    yyjson_val* root = yyjson_doc_get_root(doc);
    bool ok = false;
    if (root && yyjson_is_obj(root)) {
        yyjson_val* st = yyjson_obj_get(root, "state");
        if (yyjson_is_str(st) && yyjson_get_str(st) && *yyjson_get_str(st)) {
            out.state = yyjson_get_str(st);
            if (yyjson_val* s = yyjson_obj_get(root, "since_ms"); yyjson_is_int(s))
                out.since_ms = yyjson_get_sint(s);
            else if (yyjson_is_num(s))
                out.since_ms = (int64_t)yyjson_get_num(s);
            if (yyjson_val* r = yyjson_obj_get(root, "last_reason");
                yyjson_is_str(r) && yyjson_get_str(r))
                out.last_reason = yyjson_get_str(r);
            out.valid = true;
            ok = true;
        }
    }
    yyjson_doc_free(doc);
    return ok;
}

}  // namespace xi
