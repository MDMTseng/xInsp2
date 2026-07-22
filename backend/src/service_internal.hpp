//
// service_internal.hpp — PRIVATE shared surface for the split service_*.cpp TUs.
//
// This header exists ONLY to let service_main.cpp be mechanically split into
// several cohesive translation units WITHOUT any behavior change. It declares
// the process-wide Engine state, the shared thread_local globals, the shared
// structs / enums / constants, and forward-declarations of every helper that is
// called across the module boundary (each such helper had `static` removed at
// its single definition site — it now has external linkage). Truly file-local
// helpers keep `static` inside their own .cpp and are NOT declared here.
//
// NOT a public API. Do not include from outside backend/src/service_*.cpp.
//
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <yyjson.h>
#include <xi/xi_image.hpp>
#include <xi/xi_protocol.hpp>
#include <xi/xi_project.hpp>
#include <xi/xi_plugin_manager.hpp>
#include <xi/xi_quiesce_token.hpp>
#include <xi/xi_script_loader.hpp>
#include <xi/xi_inflight_runs.hpp>
#include <xi/xi_trigger_bus.hpp>
#include <xi/xi_emit_gate.hpp>
#include <xi/xi_ws_server.hpp>
#include <xi/xi_health.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_seh.hpp>
#include <xi/xi_crash_dump.hpp>

#include "xi_result_class.hpp"   // XI_SYS_* band + outcome_class_for_code (shared with runner_main)

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace xp = xi::proto;
using xi::seh_exception;

// ---- Engine: the host's process-wide mutable state -------------------------
static constexpr int WD_SLOTS = 64;   // max concurrent in-flight inspects tracked (Engine::wd_deadlines size)

// Latest status per component (see set_status_internal). Defined here so Engine
// can hold `std::map<std::string, StatusEntry> status;` by value.
struct StatusEntry { std::string text; int64_t ts_ms = 0; uint64_t seq = 0; };

// Ring of recently-surfaced errors (see push_recent_error). Defined here so
// Engine can hold `std::deque<RecentError> recent_errors;` by value.
struct RecentError {
    int64_t     ts_ms = 0;
    std::string source;     // "rsp" / "log" / "event"
    std::string message;
    int64_t     cmd_id  = 0;   // 0 if unknown
    int64_t     run_id  = 0;   // 0 if unknown
};

// GroupLane is fully defined in service_main.cpp (it depends on EmitGate etc.).
// Engine only holds shared_ptr<GroupLane>, so a forward declaration suffices.
struct GroupLane;

struct Engine {
    std::atomic<int64_t> run_id{0};
    xi::script::LoadedScript script;
    std::mutex script_mu;
    std::atomic<int64_t> script_generation{0};
    // U2 (docs/new_gen/16): the kv channel — canonical-mp BYTES (may contain
    // NULs; std::string used as a plain byte bag), host-opaque like the JSON
    // above. Empty = no captured store (get_kv returned 0). In-memory only,
    // exactly like the Record channel: rides hot reloads, not restarts.
    std::string persistent_kv_bytes;
    int persistent_kv_schema = 0;
    std::unordered_map<std::string, std::string> param_cache;
    std::unordered_map<std::string, std::string> instance_def_cache;
    std::atomic<bool> continuous{false};
    std::atomic<int> continuous_fps{10};
    std::atomic<int> timer_interval_ms{100};
    std::thread timer_thread;
    std::mutex run_mu;
    xi::InflightRuns inflight;
    std::atomic<int> watchdog_ms{0};
    std::atomic<int64_t> wd_deadlines[WD_SLOTS];
    std::atomic<int> watchdog_trips{0};
    std::thread watchdog_thread;
    std::atomic<bool> watchdog_run{false};
    std::atomic<xi::ws::Server*> srv_for_bp{nullptr};
    std::mutex status_mu;
    std::map<std::string, StatusEntry> status;
    std::atomic<uint64_t> status_seq{0};
    std::string include_dir;
    std::string work_dir;
    std::string plugins_dir;
    std::string opencv_dir;
    std::string turbojpeg_root;
    std::string ipp_root;
    std::string tc_vcvars;
    std::string project_folder;
    std::string include_dir_default;
    DLL_DIRECTORY_COOKIE proj_dll_dir = nullptr;
    xi::PluginManager plugin_mgr;
    std::atomic<bool> should_exit{false};
    std::atomic<bool> teardown_done{false};
    std::mutex recent_errors_mu;
    std::deque<RecentError> recent_errors;
    std::atomic<uint64_t> dropped_lifetime{0};
    std::atomic<uint64_t> high_watermark_lifetime{0};
    // Malformed / unparseable command envelopes rejected by the dispatch shell
    // (review 09 finding 2). Process-uptime cumulative; surfaced by dispatch_stats.
    std::atomic<uint64_t> malformed_cmd_rejected{0};
    std::vector<std::shared_ptr<GroupLane>> lanes;
    std::mutex lanes_mu;
    std::string default_group_snapshot;
    std::string boot_id;
    std::string station_id;
};
extern Engine g_eng;

// ---- Dispatch groups: per-group worker lanes (gated on parallelism.groups) --
// Full definition here (Engine holds shared_ptr<GroupLane>; cmd:dispatch_stats
// reads its members). Depends on EmitGate (xi_emit_gate.hpp, included above).
struct GroupLane {
    xi::ProjectInfo::DispatchGroup cfg;
    std::deque<xi::TriggerEvent>   q;
    std::mutex                     mu;
    std::condition_variable        cv;           // WORKERS wait here (notify_one on push/pop)
    // overflow:"block" back-pressure: producers park HERE (not on cv) waiting for a
    // free slot, so a freed slot wakes a producer WITHOUT waking a worker (and vice
    // versa). Kept separate from cv precisely so the worker path can stay notify_one.
    std::condition_variable        cv_not_full;
    // depth=0 RENDEZVOUS generation counter (guarded by `mu`): bumped by a worker
    // each time it DEQUEUES an event. A depth=0 producer snapshots this under the
    // lock before depositing, then waits until it advances — i.e. until *its* event
    // was taken (not merely until the slot is empty, which interleaved producers
    // would confuse). Only meaningful for queue_depth==0 lanes; harmless otherwise.
    uint64_t                       taken_count = 0;
    // Per-lane death flag (guarded by `mu`, like taken_count — every reader holds
    // lane->mu). Set true by the stop paths (stop_dispatch_pool_/stop_group_pool_)
    // in the same critical section that notifies this lane's cvs. Producers check
    // THIS — not only the global g_eng.continuous — after locking mu: a stop→resume
    // cycle (hot-recompile via DispatchPoolGuard) re-arms the GLOBAL flag while
    // g_eng.lanes holds NEW lanes, so a producer holding a stale shared_ptr to an
    // OLD lane would otherwise pass the global re-check and deposit into (or park
    // forever on) a dead, worker-less lane (lane-ABA; frame loss / pack-ref leak /
    // teardown deadlock). Fresh lanes start unstopped; the flag is never cleared.
    bool                           stopped = false;
    std::vector<std::thread>       workers;
    std::atomic<uint64_t>          running{0};
    std::atomic<uint64_t>          dropped{0};
    std::atomic<uint64_t>          high_watermark{0};
    bool                           ordered{false};
    std::atomic<int64_t>           seq_next{0};
    xi::EmitGate                   gate;
    std::atomic<int64_t>           next_allowed_us{0};
};

// ---- shared thread_local globals (defined in exactly one TU) ---------------
// g_staged      — defined in service_sinks.cpp
// g_current_trigger — defined in service_sinks.cpp
// g_run_result  — defined in service_result.cpp
struct StagedEmit {
    std::string      target;   // destination sink instance name
    xi::TriggerEvent rec;      // carries one xi_pack_handle (frames + meta doc live in the pack); host owns that ref
};
extern thread_local std::vector<StagedEmit> g_staged;

// staged-sink drain / flush (definitions in service_sinks.cpp).
void drain_staged_emits_();
void flush_staged_emits_(int64_t run_id);
// RAII backstop: drains any staged-but-unflushed sink calls on scope exit.
struct StagedEmitGuard { ~StagedEmitGuard() { drain_staged_emits_(); } };

// Watchdog slot arm/disarm (definitions in service_sinks.cpp).
int  wd_arm(int64_t deadline);
void wd_disarm(int slot);
extern thread_local const xi::TriggerEvent* g_current_trigger;

struct RunResult { int code = 0; std::string msg; bool set = false; };
extern thread_local RunResult g_run_result;

// ---- A4 explicit per-run context -------------------------------------------
// The ONE explicit carrier that retired the ambient run_id / frame_path TLS and
// the g_trigger_ctx_ relational marker. Installed on the dispatch thread by
// RunContextScope for the whole inspect, and carried onto workers so
// xi::run_id() / xi::current_frame_path() / xi::result() are correct on ANY
// worker thread (closing the spawn gap) and FAIL LOUD off a run (g_run_ctx ==
// nullptr — the single presence check). TWO propagation shapes, by worker
// lifetime:
//   * xi::async / xi::parallel_for ALWAYS join before returning, so the installing
//     dispatch frame outlives the workers — they inherit the POINTER by value
//     (run_ctx get/set thunks), zero-copy.
//   * xi::spawn_worker is fire-and-forget and may OUTLIVE the inspect, so a pointer
//     into the dispatch frame would dangle. It instead installs a heap SNAPSHOT the
//     worker OWNS (run_ctx snapshot/free thunks): run_id + frame_path copied by
//     value, result_slot = nullptr (a detached worker must not route a verdict into
//     a frame that may be gone — result_cb no-ops it; and push() is rejected off
//     it). Structurally UAF-free — no "must not read after the inspect" convention.
struct RunContext {
    long long        run_id      = 0;
    std::string      frame_path;
    RunResult*       result_slot = nullptr;   // the RUN's verdict slot; nullptr ⇒ detached snapshot (no routing)
    std::thread::id  owner_tid;               // the dispatch thread that owns g_staged
    bool             had_trigger = false;     // a triggered frame (g_current_trigger != null)
};
extern thread_local const RunContext* g_run_ctx;   // null ⇒ no live run on this thread

// RAII: install `ctx` as this dispatch thread's run context for the inspect, and
// restore the previous pointer on exit. Fills result_slot = &g_run_result and
// owner_tid = this thread. Not nested (one inspect per dispatch thread at a time).
struct RunContextScope {
    RunContext        ctx;
    const RunContext* prev;
    RunContextScope(long long run_id, std::string frame_path, bool had_trigger);
    ~RunContextScope();
    RunContextScope(const RunContextScope&) = delete;
    RunContextScope& operator=(const RunContextScope&) = delete;
};

// Fail-loud on an off-run read/write (the ONE presence check): abort in Debug,
// warn-once + safe sentinel in Release. `what` names the offending accessor.
void run_context_fail_loud_(const char* what);

// run_ctx thunks wired into the script DLL (xi_script_set_run_ctx_callbacks).
// get/set marshal the opaque RunContext* for the async/parallel_for pointer path;
// run_id / frame_path read the installed context's fields for xi::run_id() /
// xi::current_frame_path() (with the fail-loud presence check). snapshot / install
// / free are the spawn_worker BY-VALUE path: snapshot allocates a worker-owned heap
// copy of the current context (result_slot nulled; owner_tid KEPT as the parent
// dispatch thread's — Wave-2 #5 A4 symmetry, so the F4 off-thread trigger-read
// detection fires on a spawn_worker exactly like on async/parallel_for) on the
// SPAWNING thread; install installs it on the worker; free releases it when the
// worker exits.
const void* run_ctx_get_cb();
void        run_ctx_set_cb(const void* p);
long long   run_ctx_run_id_cb();
const char* run_ctx_frame_path_cb();
void*       run_ctx_snapshot_cb();               // spawning thread → worker-owned heap snapshot (null if off-run)
void        run_ctx_install_worker_cb(void* s);  // worker thread → install `s` (parent owner_tid kept)
void        run_ctx_free_cb(void* s);            // worker thread → free the snapshot

// ---- shared constants ------------------------------------------------------
// Framework system-fail band (XI_SYS_* / kResultSystemBand / kRunResultSchema /
// outcome_class_for_code) lives in xi_result_class.hpp — shared with the
// headless runner. See docs/roadmap/run-result.md.
static constexpr size_t       kRecentErrorsCap  = 64;
static const int              WATCHDOG_EXIT_CODE = 0x5744;  // 'WD' — backend self-exit on a hard trip
// Hard ceiling on a whole-file slurp that gets embedded verbatim in a command
// reply (get_dashboard, crash_reports — review 09 finding 4). Well under the
// 16 MiB WS message cap, so a pathological/corrupt file can neither drive a
// bad_alloc (→ whole-backend death via the dispatch shell) nor blow the frame.
static constexpr size_t       kMaxInlineFileBytes = 8u * 1024u * 1024u;

// Read a file into `content` with a hard size cap. Returns false if it could not
// be opened. Sets `truncated` = true (and leaves `content` empty) when the file
// exceeds `cap` — the caller must NOT embed a partial body (it would be invalid
// JSON); it reports the truncation instead. Defined in service_cmd_observability.cpp.
bool read_file_capped(const std::filesystem::path& p, size_t cap,
                      std::string& content, bool& truncated);

// trigger_id → 32-char lowercase hex (used for boot_id + run_result trigger_id).
std::string trigger_id_hex(xi_trigger_id id);

// ---- host-tracked instance state -------------------------------------------
using xi::InstState;
const char* inst_state_str(InstState s);
void set_inst_state(const std::string& name, InstState s, const std::string& err = "");
void clear_inst_state();

// ---- crash breadcrumb thin forwarders --------------------------------------
xi::crash::Context& crash_ctx();
inline void crash_set(char* dst, size_t n, const char* src) { xi::crash::set(dst, n, src); }
inline void crash_set_phase(const char* phase) { xi::crash::set_phase(phase); }
void reserve_fault_stack();

// ---- RAII: current-trigger scope (defined in service_sinks.cpp) ------------
struct CurrentTriggerScope {
    xi::TriggerEvent& ev_;   // non-const: dtor releases the event's pack ref (release_trigger_event_)
    explicit CurrentTriggerScope(xi::TriggerEvent& ev);
    ~CurrentTriggerScope();
    CurrentTriggerScope(const CurrentTriggerScope&) = delete;
    CurrentTriggerScope& operator=(const CurrentTriggerScope&) = delete;
};

// ---- shared response / logging helpers -------------------------------------
double  now_seconds();
int64_t now_ms_();
void send_rsp_ok(xi::ws::Server& srv, int64_t id, std::string data_json = "");
void send_rsp_err(xi::ws::Server& srv, int64_t id, std::string err);
// Error rsp WITH a data payload (diagnostics / partial results). Owns the
// recent-errors push exactly like the plain overload — root cause (Wave-2 #2):
// handlers that needed data_json hand-built a raw `xp::Rsp{ok:false}` and half
// of them forgot push_recent_error, so compile/export/recompile failures were
// invisible to cmd:recent_errors. No handler builds a raw error Rsp anymore.
void send_rsp_err(xi::ws::Server& srv, int64_t id, std::string err, std::string data_json);
void push_recent_error(std::string source, std::string message,
                       int64_t cmd_id = 0, int64_t run_id = 0);
void emit_error_log(xi::ws::Server& srv, const std::string& msg, int64_t run_id = 0);
void send_hello(xi::ws::Server& srv);

// ---- status registry -------------------------------------------------------
void set_status_internal(const std::string& who, const char* text);
void status_cb(const char* text);

// ---- per-run result --------------------------------------------------------
void result_cb(int code, const char* msg);
void emit_run_result(xi::ws::Server& srv, int code, const std::string& msg,
                     int64_t run_id, int64_t ms,
                     const std::string& source, const std::string& group,
                     const std::string& trigger_id = std::string(),
                     const char* cls = nullptr,
                     const char* reason_code = nullptr,
                     int64_t script_generation = 0);

// Release every host resource a finished trigger event owns (image + doc refs).
void release_trigger_event_(xi::TriggerEvent& ev);

// RAII: release the event on scope exit UNLESS dismiss()ed (handed off to a lane).
struct TriggerEventReleaser {
    xi::TriggerEvent* ev_;   // null ⇒ dismissed (handed off)
    explicit TriggerEventReleaser(xi::TriggerEvent& ev) : ev_(&ev) {}
    void dismiss() { ev_ = nullptr; }
    ~TriggerEventReleaser() { if (ev_) release_trigger_event_(*ev_); }
    TriggerEventReleaser(const TriggerEventReleaser&) = delete;
    TriggerEventReleaser& operator=(const TriggerEventReleaser&) = delete;
};

// ---- script host_api + use()/trigger/owner callbacks -----------------------
// These are cast to void* at their (compile_and_load) use site; the EXACT
// signatures must be preserved. Definitions stay in service_main.cpp.
struct CurrentTriggerInfoC {        // mirrors xi::CurrentTriggerInfo (xi_use.hpp)
    xi_trigger_id id;
    int64_t       timestamp_us;
    int32_t       is_active;
    int32_t       _pad;             // align dequeued_at_us to 8 bytes
    int64_t       dequeued_at_us;   // worker-stamped on dequeue from its lane
};
const xi_host_api* script_host_api_();
int  use_exchange_cb(const char* name, const char* cmd, char* rsp, int rsplen);
// polaris2 Gate P2: xi::use(...).process(ScriptPack) → the target's xi.pack@1
// door. 0 ok (*out = new sealed handle the script owns); -1 no such instance;
// -2 door crashed/threw; -3 quarantined; -4 no pack door. (service_sinks.cpp)
int  use_pack_process_cb(const char* name, xi_pack_handle in, xi_pack_handle* out);
// polaris2 gate P2 (expose-from-script): xi::use(sink).push(pack) — deliver a
// script-held sealed pack to a named instance's xi.pack@1 door. Sink targets
// are staged (frame-ordered flush, like use_process_cb); others run inline.
int  use_push_pack_cb(const char* name, xi_pack_handle pack);
uint32_t owner_get_cb();
void     owner_set_cb(uint32_t id);
void     trigger_info_cb(CurrentTriggerInfoC* out);

// ---- toolchain / project helpers -------------------------------------------
void resolve_toolchain_(const std::string& folder);
void read_script_deps_(const std::string& folder,
                       std::vector<std::string>& include_dirs,
                       std::vector<std::string>& link_libs,
                       int& openmp_max_threads,
                       bool& allow_raw_omp);
void set_project_dll_search_(const std::string& folder);
bool apply_process_priority_(const std::string& cls);

// ---- instance crash breadcrumbs --------------------------------------------
void note_instance_crash_(const char* name, const char* why);
void stamp_culprit_(const char* instance, const std::string& plugin);

// ---- item-14 caught-fault policy (defined in service_sinks.cpp) -------------
// Shared by every plugin-entering boundary via guarded_plugin_call below.
void apply_on_fault_policy_(const char* name, xi::CAbiInstanceAdapter* adapter);
void apply_pending_reinit_(const char* name, xi::CAbiInstanceAdapter* adapter);

// ---- guarded_plugin_call: the ONE plugin-entry fault boundary ----------------
// Root cause (Wave-2 #1): the item-14 six-step ritual — quarantined? gate →
// pending-reinit apply → re-check → stamp_culprit_ → try{enter plugin} →
// catch(seh){note_instance_crash_ + apply_on_fault_policy_ +
// recover_seh_stack_or_die} / catch(std){note + policy} — was HAND-COPIED at
// every plugin-entering site and drifted. Worst drift: cmd_prepare_instance_ /
// cmd_commit_group_ caught seh_exception only via `catch (const std::exception&)`
// (seh_exception derives from it), so recover_seh_stack_or_die never ran — after
// a plugin STACK_OVERFLOW the WS thread's stack guard page stayed CONSUMED and
// the next deep call on that thread corrupted memory instead of faulting; those
// sites also skipped the quarantine gate and all crash bookkeeping, so a
// prepare()/commit()-only crash-loop never tripped health/reinit/refuse. This
// helper owns the whole ritual; real per-site differences are explicit
// parameters, not divergent copies.
//
// `gate_quarantined`: whether a quarantined instance is REFUSED without entering
// plugin code. TRUE for the data/exchange plane (process/pack door/exchange —
// exactly what item-14 quarantine exists to stop). FALSE for the config-plane
// re-enable surface: set_def / commit are the DOCUMENTED operator un-quarantine
// path (their success path runs set_inst_state(name, Active), which lifts the
// gate — see set_inst_state in service_dispatch.cpp); gating them would make an
// on_fault=refuse quarantine unrecoverable through its own documented remedy.
// get_def is also ungated: reading the faulted config is how an operator repairs
// it. prepare() is ALSO ungated (round-3 S1): it was briefly gated on the theory
// that its success never sets Active so it "can't lift a quarantine", but for a
// STAGED plugin the documented on_fault=refuse remedy is prepare_instance →
// commit_group — gating prepare dead-ended that recovery at step 1. prepare is
// config-plane staging, exactly the surface quarantine must leave open.
//
// The on-fault POLICY (apply_on_fault_policy_) runs unconditionally on a caught
// crash/throw when the instance is a C-ABI adapter: every previously-complete
// site (use_push_pack_inline_ / use_exchange_cb / use_pack_process_cb /
// cmd_exchange_instance_) applied it in both catch arms, so there is no
// per-site fork to preserve. A null `adapter` (non-C-ABI instance) skips the
// gates and policy but keeps the culprit stamp + catch + stack recovery.
struct PluginCallResult {
    enum class Kind { Ok, Quarantined, Crashed, Threw };
    Kind        kind = Kind::Ok;
    unsigned    seh_code = 0;   // Kind::Crashed only
    std::string what;           // e.what() text (Crashed / Threw; "non-std exception" for catch(...))
    bool ok() const { return kind == Kind::Ok; }
};

template <class Fn>
PluginCallResult guarded_plugin_call(const char* name,
                                     xi::CAbiInstanceAdapter* adapter,
                                     const std::string& plugin,
                                     const char* what,           // entered surface, e.g. "exchange()" / "pack door"
                                     bool gate_quarantined,
                                     Fn&& fn) {
    using K = PluginCallResult::Kind;
    PluginCallResult r;
    if (adapter) {
        if (gate_quarantined && adapter->quarantined()) { r.kind = K::Quarantined; return r; }
        if (adapter->reinit_pending()) {
            apply_pending_reinit_(name, adapter);
            if (gate_quarantined && adapter->quarantined()) { r.kind = K::Quarantined; return r; }
        }
    }
    stamp_culprit_(name, plugin);
    try {
        fn();
        return r;
    } catch (const seh_exception& e) {
        std::fprintf(stderr, "[xinsp2] %s '%s' crashed: 0x%08X (%s)\n",
                     what, name, e.code, e.what());
        char why[96]; std::snprintf(why, sizeof(why), "%s crashed: 0x%08X", what, e.code);
        note_instance_crash_(name, why);
        if (adapter) apply_on_fault_policy_(name, adapter);
        // Swallowed on a surviving thread — restore the stack guard page after an
        // overflow (or hard-exit for respawn) BEFORE returning toward deep code.
        xi::recover_seh_stack_or_die(e.code, what);
        r.kind = K::Crashed; r.seh_code = e.code; r.what = e.what();
        return r;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[xinsp2] %s '%s' threw: %s\n", what, name, e.what());
        char why[96]; std::snprintf(why, sizeof(why), "%s threw an exception", what);
        note_instance_crash_(name, why);
        if (adapter) apply_on_fault_policy_(name, adapter);
        r.kind = K::Threw; r.what = e.what();
        return r;
    } catch (...) {
        std::fprintf(stderr, "[xinsp2] %s '%s' threw a non-std exception\n", what, name);
        char why[96]; std::snprintf(why, sizeof(why), "%s threw an exception", what);
        note_instance_crash_(name, why);
        if (adapter) apply_on_fault_policy_(name, adapter);
        r.kind = K::Threw;
        // Round-3 S5: leave a real message — converted call sites append r.what
        // to user-facing errors ("xxx error: " + r.what) and an empty tail read
        // as a truncated reply.
        r.what = "non-std exception";
        return r;
    }
}

// ---- guarded_script_call: the script-DLL sibling of guarded_plugin_call -----
// Root cause (round-3 W2 #6): the compile_and_load swap-time replay sites
// (set_instance_def replay, kv migrate-restore, kv plain-restore) hand-copied
// the same try{enter script DLL}catch(seh){stderr + recover_seh_stack_or_die}
// catch(std){stderr} ritual three times, with the crash/threw log lines one
// edit away from drifting. A script-DLL call has NO adapter, quarantine gate,
// or on-fault policy — guarded_plugin_call's item-14 machinery genuinely does
// not apply — so this is a deliberately smaller helper beside it, not a
// parametrization of it.
//   `label`       — the human stderr identity; may carry per-item detail
//                   (e.g. "replay set_instance_def 'blobs'").
//   `recover_ctx` — the recover_seh_stack_or_die context (the site class,
//                   without per-item detail — matches the pre-helper strings).
//   `fn`          — returns the thunk's int rc; nonzero maps to Refused. A
//                   caller that ignores the rc (set_instance_def's
//                   best-effort replay) just treats Refused like Ok.
// Deliberately NO catch(...): the blocks this replaces let a non-std throw
// propagate to the dispatch shell's top-level guard, and that stays true.
enum class ScriptCallOutcome { Ok, Refused, Crashed, Threw };

template <class Fn>
inline ScriptCallOutcome guarded_script_call(const std::string& label,
                                             const char* recover_ctx, Fn&& fn) {
    try {
        return fn() == 0 ? ScriptCallOutcome::Ok : ScriptCallOutcome::Refused;
    } catch (const seh_exception& e) {
        std::fprintf(stderr, "[xinsp2] %s crashed: 0x%08X (%s) — skipped\n",
                     label.c_str(), e.code, e.what());
        // Swallowed on a surviving thread — restore the stack guard page after
        // an overflow (or hard-exit for respawn) BEFORE returning toward deep code.
        xi::recover_seh_stack_or_die(e.code, recover_ctx);
        return ScriptCallOutcome::Crashed;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[xinsp2] %s threw: %s — skipped\n",
                     label.c_str(), e.what());
        return ScriptCallOutcome::Threw;
    }
}

// ---- script thunk grow-retry (Wave-2 #4) -------------------------------------
// Root cause: the script DLL buffer protocol (`n = fn(buf, len); n < 0 ⇒ grow to
// -n and retry`) was hand-rolled ~9x across the cmd handlers, and the copies
// FORKED on what -1 means. The contract (xi_script_support.hpp / xi_kv.hpp):
//   * xi_script_exchange_instance / xi_script_get_instance_def return -1 for
//     "instance not found" (terminal) and -needed for a too-small buffer. A
//     GENUINE -needed of -1 cannot reach us: needed==1 only overflows when
//     buflen < 2, and every caller starts with a KiB-scale buffer (and the
//     retry buffer is always >= 1024+). So for those thunks -1 is TERMINAL —
//     pass minus_one_is_terminal = true (retrying it was a harmless but wrong
//     extra call in most old copies; cmd_get_instance_def_ had the correct fork).
//   * xi_script_list_params / list_instances / kv_get have NO -1 error return —
//     every negative is -needed — pass minus_one_is_terminal = false.
// The resize widens through int64 first: -(int64_t)n, because -INT_MIN is UB.
template <class Buf, class Fn>
inline int script_grow_retry(Buf& buf, bool minus_one_is_terminal, Fn&& fn) {
    int n = fn(buf.data(), (int)buf.size());
    if (n < 0 && !(minus_one_is_terminal && n == -1)) {
        buf.resize((size_t)(-(int64_t)n) + 1024);
        n = fn(buf.data(), (int)buf.size());
    }
    return n;
}

// ---- inspection entry ------------------------------------------------------
void run_one_inspection(xi::ws::Server& srv,
                        int frame_hint = 1,
                        int64_t run_id = 0,
                        const std::string& frame_path = "",
                        int64_t emit_seq = -1,
                        xi::EmitGate* gate = nullptr);   // null = no ordering gate

// ---- dispatch pool lifecycle -----------------------------------------------
void spawn_group_pool_(xi::ws::Server* srv_ptr, int interval_ms);
void stop_group_pool_();
void stop_dispatch_pool_();
void install_trigger_sink_(xi::ws::Server* srv);
void controlled_shutdown_teardown_();
// Project-boundary reset (Wave-2 #3, defined in service_sinks.cpp): drop the
// bus sink + per-source emit-time map AND clear the script replay shadows
// (param_cache / instance_def_cache / persistent kv). One primitive for the
// documented cross-project leak class — previously duplicated verbatim in
// open_project / close_project (and absent from create_project, which also
// replaces the project).
void reset_project_boundary_state_();
#ifdef _WIN32
BOOL WINAPI console_ctrl_handler_(DWORD type);
#endif

// ---- dispatch quiesce guard (used by lifecycle-op cmd handlers) ------------
struct DispatchPoolGuard {
    bool            was_continuous = false;
    int             prior_fps = 10;
    bool            quiesced = false;
    xi::ws::Server* srv = nullptr;
    bool            armed_ = true;
    bool            paused_launches_ = false;
    bool            restore_sink_    = false;

    DispatchPoolGuard() = default;
    DispatchPoolGuard(DispatchPoolGuard&& o) noexcept { *this = std::move(o); }
    DispatchPoolGuard& operator=(DispatchPoolGuard&& o) noexcept {
        was_continuous = o.was_continuous; prior_fps = o.prior_fps;
        quiesced = o.quiesced; srv = o.srv; armed_ = o.armed_;
        paused_launches_ = o.paused_launches_; restore_sink_ = o.restore_sink_;
        o.armed_ = false;
        return *this;
    }
    DispatchPoolGuard(const DispatchPoolGuard&) = delete;
    DispatchPoolGuard& operator=(const DispatchPoolGuard&) = delete;
    ~DispatchPoolGuard() { resume(); }

    // Proof-of-quiesce capability (xi_quiesce_token.hpp): pass `guard.token()`
    // to every destructive PluginManager method. Only this guard (friend) and
    // the explicit QuiesceToken::assert_no_dispatch() escape hatch can mint
    // one — a lifecycle op that forgets the quiesce guard no longer compiles.
    const xi::QuiesceToken& token() const { return token_; }

    void resume() {
        if (!armed_) return;
        armed_ = false;
        // The launch pause is ALWAYS released here — at scope end, never early
        // (skip_resume() deliberately cannot release it; see O2 below).
        if (paused_launches_) { g_eng.inflight.unpause(); paused_launches_ = false; }
        if (restore_sink_ && srv) { install_trigger_sink_(srv); restore_sink_ = false; }
        if (was_continuous && quiesced) {
            bool trig_only = prior_fps <= 0;
            g_eng.continuous_fps = trig_only ? 0 : prior_fps;
            g_eng.continuous = true;
            int interval_ms = trig_only ? 0 : std::max(1, 1000 / std::max(prior_fps, 1));
            spawn_group_pool_(srv, interval_ms);
            std::fprintf(stderr, "[xinsp2] continuous mode resumed\n");
        }
    }
    // skip_resume() (formerly dismiss()): the op ends or replaces the stream
    // (open/close_project) or must leave dispatch stopped (partial
    // commit_group) — so on destruction do NOT respawn continuous mode and do
    // NOT re-install the bus sink. Unlike the old dismiss(), it does NOT
    // release the launch pause: that is ALWAYS done in the destructor
    // (resume()), never early. Releasing the pause before scope end let a
    // straggler source-emit one-shot (already past the sink read) launch into
    // a just-unmapped DLL mid-teardown — the O2 use-after-unload class. With
    // this split that bug is unwritable: the pause outlives every statement
    // in the guarded scope by construction.
    void skip_resume() {
        was_continuous = false;   // don't respawn continuous mode at scope end
        restore_sink_  = false;   // don't re-install the bus sink either
    }

private:
    // Minted via friendship (private ctor). Lives exactly as long as the
    // quiesce window this guard represents.
    xi::QuiesceToken token_;
};

DispatchPoolGuard quiesce_dispatch_for_lifecycle_op_(const char* op_name, xi::ws::Server* srv);

// ---- command handlers (dispatch table entries; defined across service_cmd_*.cpp) ----
void cmd_ping_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_version_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_shutdown_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_compile_and_load_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_save_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_commit_working_copy_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_discard_working_copy_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_load_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_create_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_open_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_close_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_timer_fps_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_run_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_start_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_stop_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_exchange_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_prepare_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_commit_group_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_crash_reports_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_clear_crash_reports_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_watchdog_ms_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_graph_capture_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_graph_snapshot_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_get_state_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_recent_errors_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_status_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_image_pool_stats_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_dispatch_stats_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_metrics_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_open_project_warnings_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_list_params_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_param_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_list_instances_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_instance_def_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_get_instance_def_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_create_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_remove_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_rename_instance_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_save_instance_config_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_get_dashboard_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_process_priority_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_list_plugins_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_rescan_plugins_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_export_project_plugin_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_recompile_project_plugin_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_rebuild_plugins_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_get_plugin_ui_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_toolchain_health_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_toolchain_override_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_get_health_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);

// ---- health/state contract (service_health.cpp) ----------------------------
// Route HealthRegistry health_changed events to WS clients + mirror the top-level
// state to the FE's status file (`health_file`; empty disables the mirror).
// Called once at boot.
void install_health_notifier_(const std::string& health_file);
