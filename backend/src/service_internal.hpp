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
#include <xi/xi_script_loader.hpp>
#include <xi/xi_inflight_runs.hpp>
#include <xi/xi_trigger_bus.hpp>
#include <xi/xi_emit_gate.hpp>
#include <xi/xi_ws_server.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_seh.hpp>
#include <xi/xi_crash_dump.hpp>

#include <windows.h>

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
    std::string persistent_state_json = "{}";
    int persistent_state_schema = 0;
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
    std::atomic<unsigned long> inspect_tid{0};
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
    std::condition_variable        cv;
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
// g_staged      — service_sinks section (defined in service_main.cpp)
// g_current_trigger — trigger-access section (defined in service_main.cpp)
// g_run_result  — result section (defined in service_main.cpp)
struct StagedEmit {
    std::string      target;   // destination sink instance name
    xi::TriggerEvent rec;      // images map + meta_doc; host owns one ref to each
};
extern thread_local std::vector<StagedEmit> g_staged;

// staged-sink drain / flush (definitions in service_main.cpp).
void drain_staged_emits_();
void flush_staged_emits_(int64_t run_id);
// RAII backstop: drains any staged-but-unflushed sink calls on scope exit.
struct StagedEmitGuard { ~StagedEmitGuard() { drain_staged_emits_(); } };

// Watchdog slot arm/disarm (definitions in service_main.cpp).
int  wd_arm(int64_t deadline);
void wd_disarm(int slot);
extern thread_local const xi::TriggerEvent* g_current_trigger;

struct RunResult { int code = 0; std::string msg; bool set = false; };
extern thread_local RunResult g_run_result;

// ---- shared constants ------------------------------------------------------
// Framework system-fail enum: a reserved band (<= -990000) the user API refuses
// to set. See docs/roadmap/run-result.md.
enum : int {
    XI_SYS_DROPPED    = -999001,  // overflow: event dropped before it could run
    XI_SYS_CRASHED    = -999002,  // caught inspect error (throw/crash) — the run did not verdict
    XI_SYS_NO_VERDICT = -999005,  // ran to completion but script set no RESULT (was v1.1 opt-in)
};
static constexpr int          kResultSystemBand = -990000;
static constexpr size_t       kRecentErrorsCap  = 64;
static const int              WATCHDOG_EXIT_CODE = 0x5744;  // 'WD' — backend self-exit on a hard trip

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

// ---- RAII: current-trigger scope (defined in service_main.cpp) -------------
struct CurrentTriggerScope {
    xi::TriggerEvent& ev_;   // non-const: dtor reset()s the event's DocRef
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
int  use_process_cb(const char* name, const void* input_doc,
                    const uint8_t* input_data, int32_t input_len,
                    const xi_record_image* input_images, int input_image_count,
                    xi_record_out* output);
int  use_exchange_cb(const char* name, const char* cmd, char* rsp, int rsplen);
xi_image_handle use_grab_cb(const char* name, int timeout_ms);
uint32_t owner_get_cb();
void     owner_set_cb(uint32_t id);
void     trigger_info_cb(CurrentTriggerInfoC* out);
xi_image_handle trigger_image_cb(const char* source);
int32_t  trigger_sources_cb(char* buf, int32_t buflen);
int32_t  trigger_leader_cb(char* buf, int32_t buflen);
void*    trigger_meta_cb();

// ---- toolchain / project helpers -------------------------------------------
void resolve_toolchain_(const std::string& folder);
void read_script_deps_(const std::string& folder,
                       std::vector<std::string>& include_dirs,
                       std::vector<std::string>& link_libs,
                       int& openmp_max_threads);
void set_project_dll_search_(const std::string& folder);
bool apply_process_priority_(const std::string& cls);

// ---- instance crash breadcrumbs --------------------------------------------
void note_instance_crash_(const char* name, const char* why);
void stamp_culprit_(const char* instance, const std::string& plugin);

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
BOOL WINAPI console_ctrl_handler_(DWORD type);

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

    void resume() {
        if (!armed_) return;
        armed_ = false;
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
    void dismiss() {
        armed_ = false;
        if (paused_launches_) { g_eng.inflight.unpause(); paused_launches_ = false; }
    }
};

DispatchPoolGuard quiesce_dispatch_for_lifecycle_op_(const char* op_name, xi::ws::Server* srv);

// ---- command handlers (dispatch table entries; defined across service_cmd_*.cpp) ----
void cmd_ping_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_version_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_shutdown_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_compile_and_load_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_unload_script_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
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
void cmd_watchdog_status_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
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
void cmd_get_project_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_save_instance_config_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_get_dashboard_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_process_priority_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_list_plugins_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_rescan_plugins_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_unquarantine_plugin_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_load_plugin_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_export_project_plugin_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_recompile_project_plugin_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_rebuild_plugins_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_get_plugin_ui_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_toolchain_health_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
void cmd_set_toolchain_override_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed);
