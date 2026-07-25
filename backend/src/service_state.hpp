//
// service_state.hpp — PRIVATE shared ENGINE STATE for the split service_*.cpp TUs.
//
// Layer 1 of the former service_internal.hpp god-header. This is the foundation
// that every service_*.cpp TU truly shares: the process-wide Engine singleton,
// the shared thread_local run/trigger globals, the shared structs / enums /
// constants, and the low-level response / status / result / crash-breadcrumb
// helpers. It pulls ONLY the engine headers the Engine + GroupLane definitions
// need BY VALUE (inflight runs, trigger bus, emit gate, quiesce token) plus the
// std-leaf crash-dump header.
//
// The two heaviest engine surfaces the Engine used to hold by value — the
// PluginManager and the LoadedScript — now live behind an opaque Engine::EngineImpl
// (pimpl), so this header only FORWARD-DECLARES them and their definitions
// (xi_plugin_manager.hpp / xi_script_loader.hpp) stay out of the shared foundation.
// The impl + Engine ctor/dtor + the plugin_mgr()/script() accessors are defined in
// service_main.cpp (where g_eng is defined). TUs that actually call PluginManager /
// LoadedScript members include those two headers directly.
//
// It likewise deliberately does NOT pull the other heavier / narrower engine
// surfaces — xi_ws_server.hpp (Server is used only by reference/pointer here, so a
// forward declaration suffices), xi_health.hpp, xi_use.hpp, and xi_seh.hpp. TUs that
// actually call those include them directly; command-handler / dispatch-pool /
// plugin-fault machinery lives in the sibling service_cmds.hpp (which includes
// this header). See that file for the command layer.
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
#include <memory>
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
// xi_project_model.hpp: the data model (ProjectInfo/InstanceInfo/InstState/…).
// GroupLane holds `xi::ProjectInfo::DispatchGroup cfg` BY VALUE and the host-tracked
// instance-state helpers below take `xi::InstState`, so this header uses the model
// directly — it used to arrive transitively via xi_plugin_manager.hpp, which is now
// pimpl-hidden, so include the model header directly (it is a light data-only leaf).
#include <xi/xi_project_model.hpp>
#include <xi/xi_quiesce_token.hpp>
#include <xi/xi_inflight_runs.hpp>
#include <xi/xi_trigger_bus.hpp>
#include <xi/xi_emit_gate.hpp>
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

// Server is passed only by reference / held only by pointer throughout this
// header, so a forward declaration keeps the heavy xi_ws_server.hpp out of the
// shared foundation. TUs that call Server methods include <xi/xi_ws_server.hpp>.
namespace xi { namespace ws { class Server; } }

// PluginManager + LoadedScript are held inside the opaque Engine::EngineImpl (pimpl),
// so this header only forward-declares them — Engine exposes them via the
// plugin_mgr()/script() accessors below. class/struct keywords match the defining
// headers (xi_plugin_manager.hpp: `class xi::PluginManager`;
// xi_script_loader.hpp: `struct xi::script::LoadedScript`).
namespace xi { class PluginManager; }
namespace xi { namespace script { struct LoadedScript; } }

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

// GroupLane is fully defined below (it depends on EmitGate etc.). Engine only
// holds shared_ptr<GroupLane>, so a forward declaration suffices.
struct GroupLane;

struct Engine {
    // pimpl: opaque holder for the two heaviest by-value members — the
    // xi::PluginManager and the xi::script::LoadedScript — reached via the
    // plugin_mgr()/script() accessors. Declared FIRST so it destructs LAST
    // (conservative: the plugin manager / script outlive every other Engine
    // member on the never-taken static-destruction fallback path — teardown is
    // otherwise fully explicit via controlled_shutdown_teardown_). Defined,
    // together with Engine()/~Engine() and the accessors, in service_main.cpp.
    struct EngineImpl;
    std::unique_ptr<EngineImpl> impl_;

    Engine();
    ~Engine();

    // Non-const refs — callers assign through them (g_eng.script() = std::move(next))
    // and pass by ref to migrate_kv / unload_script / PluginManager methods.
    xi::PluginManager&        plugin_mgr();
    xi::script::LoadedScript& script();

    std::atomic<int64_t> run_id{0};
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

// ---- health/state contract (service_health.cpp) ----------------------------
// Route HealthRegistry health_changed events to WS clients + mirror the top-level
// state to the FE's status file (`health_file`; empty disables the mirror).
// Called once at boot. Declared in the state layer (not service_cmds.hpp) so the
// pure engine-state TU that defines it — service_health.cpp — need not pull the
// command / guarded-call surface.
void install_health_notifier_(const std::string& health_file);

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
