//
// service_result.cpp — the status registry (set_status_internal / status_cb) and
// the per-run Result surface (result_cb, trigger_id_hex, outcome_class_for_code,
// emit_run_result). Split from service_main.cpp (behavior-preserving; see
// service_internal.hpp).
//
#include <cstdio>
#include <cstring>
#include <string>

#include <yyjson.h>

#include "service_internal.hpp"

// ---- Status registry -------------------------------------------------------
// Sticky last-value status per component: instance name, or "@script" for the
// inspection script. Served to the UI via cmd:status (the delivery GUARANTEE —
// clients re-pull on every connect) + a best-effort `status` push event, and
// mirrored into the per-thread crash breadcrumb so the LAST status survives a
// crash into the report.

static int64_t status_now_ms() { return xi::wall_ms(); }   // wall: status ts

// Update the latest status for `who`. Coalesces no-op repeats (same text) so a
// component setting the same string every frame doesn't spam events. Always
// mirrors into this thread's crash breadcrumb; pushes a best-effort event.
void set_status_internal(const std::string& who, const char* text) {
    std::string t = text ? text : "";
    crash_set(crash_ctx().last_status, sizeof(crash_ctx().last_status), t.c_str());
    uint64_t seq;
    {
        std::lock_guard<std::mutex> lk(g_eng.status_mu);
        auto it = g_eng.status.find(who);
        if (it != g_eng.status.end() && it->second.text == t) return;  // coalesce
        seq = ++g_eng.status_seq;
        g_eng.status[who] = StatusEntry{t, status_now_ms(), seq};
    }
    if (auto* srv = g_eng.srv_for_bp.load(std::memory_order_acquire)) {
        std::string msg = "{\"type\":\"event\",\"name\":\"status\",\"data\":{\"source\":";
        xp::json_escape_into(msg, who);
        msg += ",\"text\":";
        xp::json_escape_into(msg, t);
        msg += ",\"seq\":" + std::to_string(seq) + "}}";
        srv->send_text(msg);
    }
}

// Installed into the script DLL (xi_script_set_status_callback) so xi::status()
// in user scripts publishes under "@script".
void status_cb(const char* text) {
    set_status_internal("@script", text);
}

// ---- Per-run Result (run_result event) --------------------------------------
// One Result per trigger: a signed status code + message. See
// docs/roadmap/run-result.md. Framework system-fail enum lives in a reserved band
// (<= -990000) the user API (xi::result) refuses to set.
// XI_SYS_* enum, RunResult struct + kResultSystemBand moved to service_internal.hpp.
// g_run_result DEFINED here (thread_local; parallel lanes don't clobber each other).
thread_local RunResult g_run_result;

// Installed into the script DLL (xi_script_set_result_callback) so xi::result()
// records the one per-run verdict. The host is the trust boundary: a user code in
// the reserved system band is NOT accepted as-is — it's recorded as NA (0) with a
// visible warning + the offending code preserved in the message, so the mistake
// surfaces instead of masquerading as a real verdict.
void result_cb(int code, const char* msg) {
    if (code <= kResultSystemBand) {
        if (auto* srv = g_eng.srv_for_bp.load(std::memory_order_acquire)) {
            xp::LogMsg lm;
            lm.level = "warn";
            lm.msg = "xi::result(" + std::to_string(code) + ") uses a reserved system "
                     "code (<= -990000); the valid ng range is -1..-989999. Recorded as "
                     "NA (0) — fix the script's result code.";
            srv->send_text(lm.to_json());
        }
        g_run_result.code = 0;   // NA, not a fake ng1
        g_run_result.msg = "[invalid result code " + std::to_string(code) + ", reserved band] ";
        g_run_result.msg += (msg ? msg : "");
        g_run_result.set = true;
        return;
    }
    g_run_result.code = code;
    g_run_result.msg.assign(msg ? msg : "");
    g_run_result.set = true;
}

// Stable schema tag for the run_result wire event (bump on a breaking change to
// the field set). Rides as an additive "schema" field so consumers can version.
static constexpr const char* kRunResultSchema = "xi.run-outcome/1";

// Format a 128-bit trigger id as a 32-char lowercase hex string ("hi" then "lo",
// each zero-padded to 16). A null id (0/0) → empty string (omitted on the wire).
std::string trigger_id_hex(xi_trigger_id id) {   // decl in header (cross-TU)
    if (id.hi == 0 && id.lo == 0) return {};
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (int shift = 60; shift >= 0; shift -= 4) s.push_back(d[(id.hi >> shift) & 0xF]);
    for (int shift = 60; shift >= 0; shift -= 4) s.push_back(d[(id.lo >> shift) & 0xF]);
    return s;
}

// Derive the outcome class string from the EXISTING signed code — a pure read,
// it never changes the numeric code. Bands: >0 → "ok"; ==0 → "na"; the reserved
// system markers map to their own classes (dropped/crashed/no_verdict); a valid
// ng code (<0 and above the reserved system band) → "ng"; anything else in the
// system band → "na". The crash/no-verdict paths now emit their own reserved
// codes, so class and code agree even when emit_run_result derives the class.
static const char* outcome_class_for_code(int code) {
    if (code == XI_SYS_DROPPED)     return "dropped";
    if (code == XI_SYS_CRASHED)     return "crashed";
    if (code == XI_SYS_NO_VERDICT)  return "no_verdict";
    if (code > 0)                return "ok";
    if (code == 0)               return "na";
    if (code > kResultSystemBand) return "ng";   // valid ng band: <0 and > -990000
    return "na";                                  // other reserved system codes
}

// Emit a `run_result` wire event. Fields ride directly in the event data (same
// envelope shape as run_finished). Used by the inspect path (run_id >= 0) and the
// drop path (run_id < 0 → omitted; code = XI_SYS_DROPPED). ms < 0 omits "ms".
//
// Identity (all ADDITIVE, omitted when empty so the wire stays compact and
// existing consumers are unaffected): trigger_id (128-bit trigger id as hex),
// boot_id + station_id (process identity), a composite inspection_id
// "<station_id>/<boot_id>/<run_id>" (only when run_id>=0), a stable schema tag,
// the derived outcome class, an optional reason_code, and an optional
// script_generation (the monotonic version of the active loaded script DLL that
// produced the result; omitted when 0/unknown). The existing fields
// (code/msg/run_id/ms/source/group) and their numeric values are UNCHANGED.
// `cls`: if non-empty, overrides the code-derived class (used by the crash path,
// which keeps code 0 but is "crashed"). `reason_code`: optional, omitted if empty.
void emit_run_result(xi::ws::Server& srv, int code, const std::string& msg,
                            int64_t run_id, int64_t ms,
                            const std::string& source, const std::string& group,
                            const std::string& trigger_id,
                            const char* cls,
                            const char* reason_code,
                            int64_t script_generation) {
    std::string data = "{\"code\":" + std::to_string(code) + ",\"msg\":";
    xp::json_escape_into(data, msg);
    if (run_id >= 0) data += ",\"run_id\":" + std::to_string((long long)run_id);
    if (ms >= 0)     data += ",\"ms\":" + std::to_string((long long)ms);
    if (!source.empty()) { data += ",\"source\":"; xp::json_escape_into(data, source); }
    if (!group.empty())  { data += ",\"group\":";  xp::json_escape_into(data, group); }
    // --- additive identity fields ---
    if (!trigger_id.empty()) { data += ",\"trigger_id\":"; xp::json_escape_into(data, trigger_id); }
    if (!g_eng.boot_id.empty()) { data += ",\"boot_id\":"; xp::json_escape_into(data, g_eng.boot_id); }
    if (!g_eng.station_id.empty()) { data += ",\"station_id\":"; xp::json_escape_into(data, g_eng.station_id); }
    if (run_id >= 0) {
        // Composite id: "<station_id>/<boot_id>/<run_id>" (station_id may be empty).
        std::string insp = g_eng.station_id + "/" + g_eng.boot_id + "/" + std::to_string((long long)run_id);
        data += ",\"inspection_id\":"; xp::json_escape_into(data, insp);
    }
    data += ",\"schema\":"; xp::json_escape_into(data, std::string(kRunResultSchema));
    data += ",\"class\":"; xp::json_escape_into(data, std::string(cls ? cls : outcome_class_for_code(code)));
    if (reason_code && *reason_code) { data += ",\"reason_code\":"; xp::json_escape_into(data, std::string(reason_code)); }
    // script_generation: monotonic version of the active loaded script DLL that
    // produced this result. Omitted when 0/unknown (no script loaded, or the
    // drop path where no run ran). Unchanged across a failed compile.
    if (script_generation > 0) data += ",\"script_generation\":" + std::to_string((long long)script_generation);
    data += "}";
    xp::Event ev;
    ev.name = "run_result";
    ev.data_json = data;
    srv.send_text(ev.to_json());
}

