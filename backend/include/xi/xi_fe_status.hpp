#pragma once
//
// xi_fe_status.hpp — the FE supervisor's authoritative status, rendered as JSON.
//
// The VS Code extension (attach mode) and any operator HMI can't ask the FE
// anything directly — the FE has no WS client/server (it stays dependency-light;
// see docs/design/fe-be-split.md). Before this, the UI inferred "backend down ->
// line safe" purely from a WebSocket *disconnect*, which can't distinguish a
// transient respawn from a latched RespawnLimitExceeded, can't show the death
// reason or respawn budget, and says nothing about the comms gateway (the
// gateway was removed 2026-05; the comms fields in FeStatus are inert/reserved
// for wire-format stability — see comms_enabled / comms_state below).
//
// The FE instead publishes its true state to a small JSON **status file** that it
// rewrites (atomically) on every transition. A reader polls/watches the file. A
// file — not a socket — keeps the FE thin and mirrors the BE's heartbeat-file
// pattern; crash-history.jsonl is its append-only sibling (the timeline), this is
// the live snapshot.
//
// This header is the PURE renderer (FeStatus::render()) so it can be unit-tested.
// The atomic file swap is the only platform-specific bit and lives in fe_main.cpp.
//
// Portable: <string>/<cstdint> + json_escape from xi_crash_history.hpp. No Win32.
//
#include <cstdint>
#include <string>

#include <xi/xi_crash_history.hpp>   // xi::json_escape
#include <xi/xi_safe_state.hpp>

namespace xi {

// The FE's live state snapshot. Mutated in place by the supervisor at each
// transition, then render()'d to the status file. Field semantics mirror the
// supervisor's own variables (in_safe_state, RespawnTracker.consecutive, etc.).
struct FeStatus {
    // "starting" (spawned, not yet serving) | "healthy" (serving, line clear)
    // | "safe" (line driven safe; recovering) | "stopped" (FE shutting down).
    std::string state        = "starting";
    std::string reason;              // safe-state reason while not healthy, else ""
    bool        latched      = false;// RespawnLimitExceeded -> awaiting manual restart
    int         consecutive  = 0;    // consecutive BE failures since last recovery
    int         respawn_max  = 0;    // the cap
    int         backend_pid  = 0;    // current BE pid (0 if none)
    int         port         = 0;
    bool        working_copy = false;
    // INERT/reserved wire-compat fields — the out-of-process comms gateway was
    // removed 2026-05. The FE never sets these; they are always false/"" at runtime.
    // Retained byte-for-byte in render() for JSON wire-format stability (the VS Code
    // extension may parse the "comms" object; test_qa_edge.cpp:257 exercises them).
    bool        comms_enabled = false;
    std::string comms_state;         // always "" now; was "up"|"down"|"gaveup" (disabled)
    // The most recent death's forensics (empty until the first death).
    bool           has_last_event = false;
    SafeStateEvent last_event;
    std::string crash_history;       // path to the JSONL timeline (for the UI)
    int64_t     ts_ms = 0;           // when this snapshot was written

    void set_event(const SafeStateEvent& ev) { last_event = ev; has_last_event = true; }

    std::string render() const {
        std::string o = "{";
        o += "\"schema\":1";
        o += ",\"ts_ms\":" + std::to_string(static_cast<long long>(ts_ms));
        o += ",\"state\":\"" + json_escape(state) + "\"";
        o += ",\"reason\":\"" + json_escape(reason) + "\"";
        o += ",\"latched\":" + std::string(latched ? "true" : "false");
        o += ",\"consecutive\":" + std::to_string(consecutive);
        o += ",\"respawn_max\":" + std::to_string(respawn_max);
        o += ",\"backend_pid\":" + std::to_string(backend_pid);
        o += ",\"port\":" + std::to_string(port);
        o += ",\"working_copy\":" + std::string(working_copy ? "true" : "false");
        o += ",\"comms\":{\"enabled\":" + std::string(comms_enabled ? "true" : "false");
        o += ",\"state\":\"" + json_escape(comms_state) + "\"}";
        o += ",\"crash_history\":\"" + json_escape(crash_history) + "\"";
        if (has_last_event) {
            const SafeStateEvent& e = last_event;
            o += ",\"last_event\":{";
            o += "\"reason\":\"" + std::string(to_string(e.reason)) + "\"";
            o += ",\"rc\":" + std::to_string(e.backend_rc);
            o += ",\"exception\":\"" + json_escape(e.exception_name) + "\"";
            o += ",\"module\":\""    + json_escape(e.faulting_module) + "\"";
            o += ",\"phase\":\""     + json_escape(e.last_phase) + "\"";
            o += ",\"report\":\""    + json_escape(e.report_path) + "\"";
            o += ",\"dump\":\""      + json_escape(e.dump_path) + "\"";
            o += ",\"ts_ms\":" + std::to_string(static_cast<long long>(e.ts_ms));
            o += "}";
        } else {
            o += ",\"last_event\":null";
        }
        o += "}";
        return o;
    }
};

} // namespace xi
