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

// ---- A4 explicit per-run context (retires the ambient run_id/frame_path TLS +
//      the g_trigger_ctx_ relational marker) --------------------------------------
// The installed context pointer for THIS thread. Set on the dispatch thread by
// RunContextScope, and re-installed on xi::async / xi::parallel_for /
// xi::spawn_worker workers via run_ctx_set_cb (the get/set thunks below). null ⇒
// no live run on this thread — the single presence check every accessor uses.
thread_local const RunContext* g_run_ctx = nullptr;

RunContextScope::RunContextScope(long long run_id, std::string frame_path, bool had_trigger)
    : prev(g_run_ctx) {
    ctx.run_id      = run_id;
    ctx.frame_path  = std::move(frame_path);
    ctx.result_slot = &g_run_result;                 // this dispatch thread's verdict slot
    ctx.owner_tid   = std::this_thread::get_id();     // the thread that owns/flushes g_staged
    ctx.had_trigger = had_trigger;
    g_run_ctx = &ctx;
}
RunContextScope::~RunContextScope() { g_run_ctx = prev; }

// The ONE presence-check fail-loud (an off-run read/write): abort in Debug,
// warn-once + safe sentinel in Release. Replaces the retired relational guards
// (F4/J4-mirror/silent-result) — a worker of a live run now has the context
// propagated, so this only fires when there is genuinely NO run on this thread.
void run_context_fail_loud_(const char* what) {
    char msg[256];
    std::snprintf(msg, sizeof(msg),
        "%s called with NO live run context on this thread — a per-run accessor "
        "(xi::run_id / xi::current_frame_path / xi::result) was used outside an "
        "inspect, or on a thread the framework did not spawn (raw std::thread / "
        "#pragma omp). Use xi::async / xi::parallel_for / xi::spawn_worker so the "
        "run context is propagated.", what);
#ifndef NDEBUG
    std::fprintf(stderr, "FATAL: [xinsp2] %s\n", msg);
    std::fflush(stderr);
    std::abort();
#else
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "ERROR: [xinsp2] %s\n", msg);
#endif
}

// run_ctx thunks wired into the script DLL. get/set marshal the opaque
// RunContext* for the spawn primitives (like the owner thunks); run_id /
// frame_path read the installed context for xi::run_id() / current_frame_path().
const void* run_ctx_get_cb() { return g_run_ctx; }
void        run_ctx_set_cb(const void* p) { g_run_ctx = static_cast<const RunContext*>(p); }
long long   run_ctx_run_id_cb() {
    if (!g_run_ctx) { run_context_fail_loud_("xi::run_id()"); return 0; }
    return g_run_ctx->run_id;
}
const char* run_ctx_frame_path_cb() {
    if (!g_run_ctx) { run_context_fail_loud_("xi::current_frame_path()"); return ""; }
    return g_run_ctx->frame_path.c_str();
}

// spawn_worker BY-VALUE path (structural UAF fix). A spawn_worker thread is
// fire-and-forget and may outlive the inspect, so it must NOT hold a pointer into
// the spawning dispatch frame. Instead it OWNS a heap snapshot: run_id + frame_path
// copied by value; result_slot NULLED (a worker that may outlive its inspect must
// not route a verdict into a frame that could already be gone — result_cb no-ops a
// null-slot context, and push_off_dispatch_thread_ rejects it). snapshot runs on
// the SPAWNING thread (g_run_ctx is the live inspect there); null when off a run
// (the worker then simply has no context). install runs on the WORKER and stamps
// owner_tid to the worker's own thread; free releases it when the worker exits.
void* run_ctx_snapshot_cb() {
    if (!g_run_ctx) return nullptr;
    auto* s = new RunContext(*g_run_ctx);
    s->result_slot = nullptr;   // detached: no live run slot to route into
    return s;
}
void run_ctx_install_worker_cb(void* s) {
    if (s) static_cast<RunContext*>(s)->owner_tid = std::this_thread::get_id();
    g_run_ctx = static_cast<const RunContext*>(s);
}
void run_ctx_free_cb(void* s) { delete static_cast<RunContext*>(s); }

// Installed into the script DLL (xi_script_set_result_callback) so xi::result()
// records the one per-run verdict. The host is the trust boundary: a user code in
// the reserved system band is NOT accepted as-is — it's recorded as NA (0) with a
// visible warning + the offending code preserved in the message, so the mistake
// surfaces instead of masquerading as a real verdict.
void result_cb(int code, const char* msg) {
    // A4 explicit-context routing (retires the F4/J4-mirror silent-result guard):
    // the verdict is written to the RUN's slot via the installed context, not to
    // whatever thread_local g_run_result the calling thread happens to own. On the
    // dispatch thread that IS &g_run_result; inside xi::parallel_for / xi::async
    // the context was propagated by value onto the worker, so its result_slot
    // still points at the dispatch thread's g_run_result — a verdict set from a
    // worker now reaches the run (the silent-NG-loss footgun is gone) instead of
    // being rejected. Off a live run (g_run_ctx == null) there is no slot to route
    // to: fail loud via the single presence check and drop the write.
    if (!g_run_ctx) { run_context_fail_loud_("xi::result()/xi::ng()"); return; }
    // Detached snapshot (a spawn_worker that may outlive its inspect — result_slot
    // is nulled by run_ctx_snapshot_cb): a real context, but no live run slot to
    // route into. A legitimate structural state, NOT an off-run bug, so no-op
    // quietly (warn once) instead of aborting — routing a verdict into a
    // possibly-gone frame is meaningless.
    if (!g_run_ctx->result_slot) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "WARN: [xinsp2] xi::result()/xi::ng() from a detached "
                "spawn_worker context — no live run to route the verdict to; ignored. "
                "Set the verdict on the inspect thread instead.\n");
        return;
    }
    RunResult& rr = *g_run_ctx->result_slot;
    if (code <= kResultSystemBand) {
        if (auto* srv = g_eng.srv_for_bp.load(std::memory_order_acquire)) {
            xp::LogMsg lm;
            lm.level = "warn";
            lm.msg = "xi::result(" + std::to_string(code) + ") uses a reserved system "
                     "code (<= -990000); the valid ng range is -1..-989999. Recorded as "
                     "NA (0) — fix the script's result code.";
            srv->send_text(lm.to_json());
        }
        rr.code = 0;   // NA, not a fake ng1
        rr.msg = "[invalid result code " + std::to_string(code) + ", reserved band] ";
        rr.msg += (msg ? msg : "");
        rr.set = true;
        return;
    }
    rr.code = code;
    rr.msg.assign(msg ? msg : "");
    rr.set = true;
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

