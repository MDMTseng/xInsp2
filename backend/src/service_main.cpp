//
// service_main.cpp — xinsp-backend.exe entry point (M2 skeleton).
//
// Responsibilities in this milestone:
//   - parse --port
//   - start the WS server
//   - handle cmd: ping, version, shutdown
//   - echo anything else as an error rsp
//
// M3 adds run + vars. M4 adds previews. M5 adds compile_and_load.
//

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <typeinfo>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cJSON.h>
#include <xi/xi.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_jpeg.hpp>
#include <xi/xi_protocol.hpp>
#include <xi/xi_cert.hpp>
#include <xi/xi_plugin_manager.hpp>
#include <xi/xi_project.hpp>
#include <xi/xi_trigger_bus.hpp>
#include <xi/xi_trigger_bridge.hpp>
#include <xi/xi_trigger_recorder.hpp>
#include <xi/xi_script_compiler.hpp>
#include <xi/xi_script_loader.hpp>
#include <xi/xi_source.hpp>
#include <xi/xi_ws_server.hpp>

#include <condition_variable>
#include <filesystem>
#include <thread>

// Minidump support (top-level crash filter). dbghelp.lib is linked
// via the CMake target. psapi for module-blame lookup.
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace xp = xi::proto;

static std::atomic<int64_t> g_run_id{0};

// Loaded user script state. When null, cmd:run returns an error.
static xi::script::LoadedScript g_script;

static std::mutex               g_script_mu;

// Persistent cross-frame state — survives DLL reloads.
static std::string g_persistent_state_json = "{}";
// Schema version of the DLL that wrote g_persistent_state_json. The
// next DLL's xi_script_state_schema_version() is compared against
// this on restore — mismatch (and both non-zero) drops the state
// rather than letting set_state default-fill into a different shape.
// 0 means "unversioned" — restore proceeds without the check.
static int         g_persistent_state_schema = 0;

// Cache of every successful `cmd:set_param` value the backend pushed
// into the live script. compile_and_load replays these into the new
// DLL via xi_script_set_param so user-tuned slider values aren't
// silently reset to file-scope defaults across a recompile. Keyed by
// param name → JSON-encoded scalar (number / bool / string token,
// same shape as set_param's `value` arg). Protected by g_script_mu.
static std::unordered_map<std::string, std::string> g_param_cache;

// --- xi::use() callback implementations ---
// These are called FROM the script DLL back INTO the backend, routing
// process/exchange/grab to the backend's InstanceRegistry.

#include <xi/xi_use.hpp>
#include <xi/xi_seh.hpp>

using xi::seh_exception;
using xi::seh_translator;

static int use_process_cb(const char* name,
                          const char* input_json,
                          const xi_record_image* input_images, int input_image_count,
                          xi_record_out* output) {
    auto inst = xi::InstanceRegistry::instance().find(name);
    if (!inst) return -1;

    // All plugins run in-process (process isolation removed 2026-05).
    // Check if it's a C ABI adapter with process_fn
    auto* adapter = dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get());
    if (adapter && adapter->process_fn()) {
        xi_record in_rec;
        in_rec.images = input_images;
        in_rec.image_count = input_image_count;
        in_rec.json = input_json;
        // adapter->process() owns the owner_id tagging (image-leak sweep) AND,
        // for a non-reentrant plugin, the per-instance lock that serializes
        // concurrent dispatch workers. We keep the SEH try/catch boundary here.
        try {
            return adapter->process(&in_rec, output);
        } catch (const seh_exception& e) {
            std::fprintf(stderr, "[xinsp2] use_process('%s') crashed: 0x%08X (%s)\n",
                         name, e.code, e.what());
            return -2;
        } catch (...) {
            std::fprintf(stderr, "[xinsp2] use_process('%s') threw exception\n", name);
            return -2;
        }
    }
    return -1;
}

static int use_exchange_cb(const char* name, const char* cmd,
                           char* rsp, int rsplen) {
    try {
        auto inst = xi::InstanceRegistry::instance().find(name);
        if (!inst) return -1;
        std::string result = inst->exchange(cmd);
        int n = (int)result.size();
        if (rsplen < n + 1) return -n;
        std::memcpy(rsp, result.data(), result.size());
        rsp[result.size()] = 0;
        return n;
    } catch (const seh_exception& e) {
        std::fprintf(stderr, "[xinsp2] use_exchange('%s') crashed: 0x%08X (%s)\n",
                     name, e.code, e.what());
        return -1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[xinsp2] use_exchange('%s') threw: %s\n", name, e.what());
        return -1;
    }
}

static xi_image_handle use_grab_cb(const char* name, int timeout_ms) {
    auto inst = xi::InstanceRegistry::instance().find(name);
    auto* src = inst ? dynamic_cast<xi::ImageSource*>(inst.get()) : nullptr;
    if (!src) return XI_IMAGE_NULL;
    xi::Image img = src->grab_wait(timeout_ms);
    if (img.empty()) return XI_IMAGE_NULL;
    return xi::ImagePool::instance().from_image(img);
}

// ---- Trigger loop state ----
// When running in continuous mode (cmd: start), a worker thread waits for
// trigger signals from image sources and calls inspect() for each frame.
static std::atomic<bool>       g_continuous{false};
// FPS the most recent cmd:start was launched with. compile_and_load
// captures this to re-arm continuous mode at the same rate after the
// reload completes — without it, mid-run hot-reload would silently
// halt the stream.
static std::atomic<int>        g_continuous_fps{10};
// Reserve stack headroom (def near write_minidump) so the crash filter can dump
// after a script STACK_OVERFLOW; called at the top of each inspect-running thread.
static void reserve_fault_stack();
// Worker thread pool. project.json `parallelism.dispatch_threads: N`
// controls the size; default 1 (current behaviour). All workers pull
// from the same g_ev_queue. A separate timer thread (`g_timer_thread`)
// pushes a synthetic empty event at the configured fps so scripts
// that don't have a trigger source still get periodic dispatch.
static std::vector<std::thread> g_worker_threads;
static std::thread              g_timer_thread;
// Result ordering (parallelism.result_order / per-group result_order). When
// ordered, each popped event gets a gapless emit sequence (assigned at dequeue
// under the queue lock so it follows arrival order) and an EmitTurn gate makes
// workers emit run_result/vars/run_finished in that order. Compute still runs
// fully parallel; only emission is serialized. In "completion" mode emit_seq is
// -1 (emit immediately, as before).
//
// An EmitGate is the cursor+lock+cv for one ordered stream. The legacy single
// pool uses g_global_gate; each dispatch-group lane owns its own (so groups don't
// serialize against each other — only within a group). Non-movable (holds a
// mutex/cv), so always referenced by pointer.
struct EmitGate {
    std::mutex              mu;
    std::condition_variable cv;
    int64_t                 next = 0;   // guarded by mu
};
static std::atomic<bool>       g_result_ordered{false};
static std::atomic<int64_t>    g_dispatch_seq{0};   // legacy pool's next emit seq (per cmd:start)
static EmitGate                g_global_gate;        // legacy single-pool ordered emission

// RAII emit-order gate. For emit_seq >= 0 (ordered mode) the ctor blocks until
// it's this sequence's turn on `gate`; the dtor advances the cursor + wakes the
// next worker — even on an exception or an error path, so a crashed inspect can't
// stall the stream. emit_seq < 0 (completion mode / cmd:run) is a no-op.
struct EmitTurn {
    EmitGate* g_;
    int64_t   seq_;
    bool      on_;
    EmitTurn(EmitGate* gate, int64_t seq) : g_(gate), seq_(seq), on_(gate && seq >= 0) {
        if (!on_) return;
        std::unique_lock<std::mutex> lk(g_->mu);
        g_->cv.wait(lk, [this] {
            return g_->next == seq_ || !g_continuous.load();
        });
    }
    ~EmitTurn() {
        if (!on_) return;
        {
            std::lock_guard<std::mutex> lk(g_->mu);
            if (g_->next == seq_) ++g_->next;   // skip if we ran early on stop
        }
        g_->cv.notify_all();
    }
    EmitTurn(const EmitTurn&) = delete;
    EmitTurn& operator=(const EmitTurn&) = delete;
};
// Serialise cmd:run dispatch threads so history / vars arrive in run_id
// order. Threads queue up here and the watchdog operates on whichever
// one is currently inside run_one_inspection — only one at a time.
static std::mutex              g_run_mu;

// Crash context — a snapshot of "what was happening" updated by the
// dispatch hot path. Read by the unhandled-exception filter to produce
// a human-readable report alongside the minidump. Pure POD + plain
// strncpy so the filter is signal-safe (no allocations, no locks).
struct CrashContext {
    uint32_t thread_id     = 0;  // owning thread (0 = slot free)
    char last_cmd[64]      {};   // last cmd handled
    char last_script[260]  {};   // last loaded script DLL path
    char last_instance[64] {};   // last instance whose plugin we called
    char last_plugin[64]   {};   // plugin name backing it
    char last_phase[32]    {};   // inspect lifecycle phase (reset/inspect/...)
    char last_status[96]   {};   // last xi::status()/set_status text on this thread
    int  last_run_id       = 0;
    int  last_frame        = 0;
};

// Per-thread crash breadcrumbs. A single global was racy under
// dispatch_threads > 1 — N concurrent inspects all wrote the same
// struct, so a crash dump could blame the wrong thread's plugin.
// Each thread claims a fixed slot (keyed by thread id) on first use;
// slots are static so they never dangle when a dispatch thread exits
// (its tid just stays recorded until reused). The crash handler walks
// all claimed slots and flags the one matching the faulting thread.
static constexpr int kMaxCrashSlots = 64;
static CrashContext            g_crash_slots[kMaxCrashSlots];
static std::atomic<uint32_t>   g_crash_slot_tid[kMaxCrashSlots];

static CrashContext& crash_ctx() {
    static thread_local int t_idx = -1;
    if (t_idx >= 0) return g_crash_slots[t_idx];
    uint32_t tid = (uint32_t)GetCurrentThreadId();
    for (int i = 0; i < kMaxCrashSlots; ++i) {
        uint32_t expected = 0;
        if (g_crash_slot_tid[i].compare_exchange_strong(
                expected, tid, std::memory_order_acq_rel)) {
            t_idx = i;
            g_crash_slots[i].thread_id = tid;
            return g_crash_slots[i];
        }
    }
    // Slots exhausted (>64 live threads ever) — fall back to slot 0.
    // Racy but never null; bounded to a pathological thread count.
    return g_crash_slots[0];
}

inline void crash_set(char* dst, size_t n, const char* src) {
    if (!dst || !src) return;
    std::strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

// Convenience for setting the current thread's inspect phase.
inline void crash_set_phase(const char* phase) {
    auto& c = crash_ctx();
    crash_set(c.last_phase, sizeof(c.last_phase), phase);
}

// Watchdog (P2.4). When > 0, inspect() calls have this many ms of wall-
// clock budget. Default 0 = disabled (back-compat). Set via
// cmd:set_watchdog_ms or --watchdog=N.
//
// Per-worker deadlines: the parallel dispatch pool (parallelism.dispatch_threads
// > 1) runs N inspects at once, so the watchdog tracks a SLOT per in-flight
// inspect (each arms a free slot on entry, clears it on exit). The monitor scans
// all slots. On a deadline breach it first asks the script to cancel cooperatively
// (a GLOBAL flag — under N>1 this aborts every in-flight frame, which is the
// intended "something's wedged, bail this round" signal); if the script ignores
// that for the grace window, the process is unrecoverable (a forced thread kill
// would leak the per-instance lock + risk heap corruption), so the backend
// exits and the FE supervisor respawns a clean one. See docs/guides/writing-a-
// script.md (Parallel dispatch) + design/fe-be-split.md.
static std::atomic<int>        g_watchdog_ms{0};
static constexpr int           WD_SLOTS = 64;     // max concurrent in-flight inspects tracked
// Per-slot inspect deadline (steady_clock epoch-ms); 0 = free. Written by the
// dispatch/run thread that owns the slot, read by the watchdog thread.
static std::atomic<int64_t>    g_wd_deadlines[WD_SLOTS];
static std::atomic<int>        g_watchdog_trips{0};
static std::thread             g_watchdog_thread;
static std::atomic<bool>       g_watchdog_run{false};
static const int               WATCHDOG_EXIT_CODE = 0x5744;  // 'WD' — backend self-exit on a hard trip

// Claim a free watchdog slot for `deadline` (steady-clock epoch-ms). Returns the
// slot index, or -1 if all slots are busy (then this inspect runs unwatched —
// only possible with >64 concurrent inspects, far beyond any real pool).
static int wd_arm(int64_t deadline) {
    for (int i = 0; i < WD_SLOTS; ++i) {
        int64_t expect = 0;
        if (g_wd_deadlines[i].compare_exchange_strong(expect, deadline)) return i;
    }
    return -1;
}
static void wd_disarm(int slot) { if (slot >= 0) g_wd_deadlines[slot].store(0); }
// True if any slot's deadline is in the past (an inspect overran its budget).
static bool wd_any_overran(int64_t now_ms) {
    for (int i = 0; i < WD_SLOTS; ++i) {
        int64_t dl = g_wd_deadlines[i].load();
        if (dl != 0 && now_ms >= dl) return true;
    }
    return false;
}

// Preview subscription (S1). Default: send every image VAR's JPEG after
// a run (back-compat). Client sets a name allow-list via cmd:subscribe
// to cut bandwidth for vars nobody is watching. Held under g_sub_mu so
// the WS thread (who mutates it) and the run dispatch thread (who reads)
// stay consistent.
static std::mutex                    g_sub_mu;
static bool                          g_sub_all = true;
static std::unordered_set<std::string> g_sub_names;

// History ring (S4). After every run we stash {run_id, ts_ms, vars_json}
// in a bounded deque so a client can scroll back through recent runs
// without re-executing. Default depth 50; client may resize via
// cmd: set_history_depth.
struct HistoryEntry { int64_t run_id; int64_t ts_ms; std::string vars_json; };
static std::mutex                  g_hist_mu;
static std::deque<HistoryEntry>    g_history;
static size_t                      g_hist_max = 50;

// Breakpoint coordination (S3). Script thread calls breakpoint_cb()
// which: (a) emits an event on the WS, (b) blocks on g_bp_cv until
// the WS thread receives `cmd: resume`. g_bp_paused is the predicate
// so spurious wakeups don't miss the signal.
static std::mutex              g_bp_mu;
static std::condition_variable g_bp_cv;
static bool                    g_bp_paused = false;
static std::string             g_bp_last_label;
static xi::ws::Server*         g_srv_for_bp = nullptr;   // set in main

// ---- Trigger access (script callbacks) ---------------------------------
// Set by the worker thread (or run_one_inspection) before invoking the
// script. The script reads via xi::current_trigger() through the three
// trigger_*_cb functions below. thread_local so multiple parallel
// dispatch threads can each have their own current trigger.
static thread_local const xi::TriggerEvent* g_current_trigger = nullptr;

// Bus event queue feeding the continuous-mode worker. Bus sink pushes
// events here; worker pops, dispatches, releases handles.
static std::deque<xi::TriggerEvent> g_ev_queue;
static std::mutex                   g_ev_mu;
static std::condition_variable      g_ev_cv;

struct CurrentTriggerInfoC {        // mirrors xi::CurrentTriggerInfo (xi_use.hpp)
    xi_trigger_id id;
    int64_t       timestamp_us;
    int32_t       is_active;
    int32_t       _pad;             // align dequeued_at_us to 8 bytes
    int64_t       dequeued_at_us;   // worker-stamped on pop from g_ev_queue
};

static void trigger_info_cb(CurrentTriggerInfoC* out) {
    if (!out) return;
    if (!g_current_trigger) { *out = {{0,0}, 0, 0, 0, 0}; return; }
    out->id             = g_current_trigger->id;
    out->timestamp_us   = g_current_trigger->timestamp_us;
    out->is_active      = 1;
    out->_pad           = 0;
    out->dequeued_at_us = g_current_trigger->dequeued_at_us;
}

static xi_image_handle trigger_image_cb(const char* source) {
    if (!g_current_trigger || !source) return XI_IMAGE_NULL;
    auto it = g_current_trigger->images.find(source);
    if (it == g_current_trigger->images.end()) return XI_IMAGE_NULL;
    // Caller (script) releases via host_api->image_release after copying
    // pixels — addref so our own release on dispatch-end doesn't free it
    // out from under them.
    xi::ImagePool::instance().addref(it->second);
    return it->second;
}

static int32_t trigger_sources_cb(char* buf, int32_t buflen) {
    if (!g_current_trigger || !buf) return 0;
    std::string out;
    bool first = true;
    for (auto& [src, h] : g_current_trigger->images) {
        if (!first) out.push_back('\n');
        first = false;
        out += src;
    }
    int32_t n = (int32_t)out.size();
    if (buflen < n + 1) return -n;
    std::memcpy(buf, out.data(), n);
    buf[n] = 0;
    return n;
}

// P2-2: expose TriggerEvent::leader_source to scripts. For policy=any the
// leader is whichever instance emitted; for leader_followers it's the
// configured leader; for all_required it's typically empty and the script
// should consult sources(). Same -needed_bytes convention as
// trigger_sources_cb so scripts can resize and retry.
static int32_t trigger_leader_cb(char* buf, int32_t buflen) {
    if (!g_current_trigger || !buf) return 0;
    const std::string& s = g_current_trigger->leader_source;
    int32_t n = (int32_t)s.size();
    if (n == 0) return 0;
    if (buflen < n + 1) return -n;
    std::memcpy(buf, s.data(), n);
    buf[n] = 0;
    return n;
}

static void breakpoint_cb(const char* label) {
    // Called from the script thread. Emit a text event, then block until
    // the WS thread sets g_bp_paused=false via `cmd: resume`.
    //
    // If we're not in continuous mode, don't park — otherwise a single
    // `cmd: run` would deadlock the WS thread, and stop/unload would
    // have to re-release after every inspect iteration. Breakpoints
    // are a continuous-mode feature.
    if (!g_srv_for_bp || !g_continuous.load()) return;
    std::string safe = label ? label : "";
    // Build event JSON with escaped label.
    std::string msg = "{\"type\":\"event\",\"name\":\"breakpoint\",\"data\":{\"label\":";
    xp::json_escape_into(msg, safe);
    msg += "}}";
    g_srv_for_bp->send_text(msg);

    std::unique_lock<std::mutex> lk(g_bp_mu);
    g_bp_paused     = true;
    g_bp_last_label = safe;
    g_bp_cv.wait(lk, []{ return !g_bp_paused; });
}

// ---- Status registry -------------------------------------------------------
// Sticky last-value status per component: instance name, or "@script" for the
// inspection script. Served to the UI via cmd:status (the delivery GUARANTEE —
// clients re-pull on every connect) + a best-effort `status` push event, and
// mirrored into the per-thread crash breadcrumb so the LAST status survives a
// crash into the report.
struct StatusEntry { std::string text; int64_t ts_ms = 0; uint64_t seq = 0; };
static std::mutex                         g_status_mu;
static std::map<std::string, StatusEntry> g_status;
static std::atomic<uint64_t>              g_status_seq{0};

static int64_t status_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Update the latest status for `who`. Coalesces no-op repeats (same text) so a
// component setting the same string every frame doesn't spam events. Always
// mirrors into this thread's crash breadcrumb; pushes a best-effort event.
static void set_status_internal(const std::string& who, const char* text) {
    std::string t = text ? text : "";
    crash_set(crash_ctx().last_status, sizeof(crash_ctx().last_status), t.c_str());
    uint64_t seq;
    {
        std::lock_guard<std::mutex> lk(g_status_mu);
        auto it = g_status.find(who);
        if (it != g_status.end() && it->second.text == t) return;  // coalesce
        seq = ++g_status_seq;
        g_status[who] = StatusEntry{t, status_now_ms(), seq};
    }
    if (g_srv_for_bp) {
        std::string msg = "{\"type\":\"event\",\"name\":\"status\",\"data\":{\"source\":";
        xp::json_escape_into(msg, who);
        msg += ",\"text\":";
        xp::json_escape_into(msg, t);
        msg += ",\"seq\":" + std::to_string(seq) + "}}";
        g_srv_for_bp->send_text(msg);
    }
}

// Installed into the script DLL (xi_script_set_status_callback) so xi::status()
// in user scripts publishes under "@script".
static void status_cb(const char* text) {
    set_status_internal("@script", text);
}

// ---- Per-run Result (run_result event) --------------------------------------
// One Result per trigger: a signed status code + message. See
// docs/design/run-result.md. Framework system-fail enum lives in a reserved band
// (<= -990000) the user API (xi::result) refuses to set.
enum : int {
    XI_SYS_DROPPED    = -999001,  // overflow: event dropped before it could run
    XI_SYS_NO_VERDICT = -999005,  // ran but script set no RESULT (v1.1 opt-in; unused in v1)
};

// The current run's result, written by the script via xi::result(code,msg)
// through result_cb. thread_local so parallel lanes don't clobber each other
// (same as g_run_frame_path_). Reset at the top of each inspect.
struct RunResult { int code = 0; std::string msg; bool set = false; };
static thread_local RunResult g_run_result;

// Lowest user-usable result code; anything <= this is the framework system-fail
// band (mirrors xi::kResultSystemBand in xi_result.hpp).
static constexpr int kResultSystemBand = -990000;

// Installed into the script DLL (xi_script_set_result_callback) so xi::result()
// records the one per-run verdict. The host is the trust boundary: a user code in
// the reserved system band is NOT accepted as-is — it's recorded as NA (0) with a
// visible warning + the offending code preserved in the message, so the mistake
// surfaces instead of masquerading as a real verdict.
static void result_cb(int code, const char* msg) {
    if (code <= kResultSystemBand) {
        if (g_srv_for_bp) {
            xp::LogMsg lm;
            lm.level = "warn";
            lm.msg = "xi::result(" + std::to_string(code) + ") uses a reserved system "
                     "code (<= -990000); the valid ng range is -1..-989999. Recorded as "
                     "NA (0) — fix the script's result code.";
            g_srv_for_bp->send_text(lm.to_json());
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

// Emit a `run_result` wire event. Fields ride directly in the event data (same
// envelope shape as run_finished). Used by the inspect path (run_id >= 0) and the
// drop path (run_id < 0 → omitted; code = XI_SYS_DROPPED). ms < 0 omits "ms".
static void emit_run_result(xi::ws::Server& srv, int code, const std::string& msg,
                            int64_t run_id, int64_t ms,
                            const std::string& source, const std::string& group) {
    std::string data = "{\"code\":" + std::to_string(code) + ",\"msg\":";
    xp::json_escape_into(data, msg);
    if (run_id >= 0) data += ",\"run_id\":" + std::to_string((long long)run_id);
    if (ms >= 0)     data += ",\"ms\":" + std::to_string((long long)ms);
    if (!source.empty()) { data += ",\"source\":"; xp::json_escape_into(data, source); }
    if (!group.empty())  { data += ",\"group\":";  xp::json_escape_into(data, group); }
    data += "}";
    xp::Event ev;
    ev.name = "run_result";
    ev.data_json = data;
    srv.send_text(ev.to_json());
}

// ---- Comms gateway client --------------------------------------------------
// Connects to the out-of-process comms gateway (xinsp-comms) over loopback and
// backs the script's xi::comms::* API. The gateway owns the PLC link; we just
// relay newline-JSON ops and buffer PLC-originated lines for poll(). A
// background reader thread keeps the inbox + link state current. See
// docs/design/comms-gateway.md.
class GatewayClient {
public:
    bool connect(int port) {
        // Winsock may not be up yet (we connect before srv.start()); WSAStartup
        // is refcounted, so an extra call here is safe.
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
        for (int attempt = 0; attempt < 15; ++attempt) {   // tolerate FE spawn race
            SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s != INVALID_SOCKET) {
                sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
                InetPtonA(AF_INET, "127.0.0.1", &a.sin_addr);
                if (::connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
                    sock_ = s; run_ = true;
                    reader_ = std::thread([this] { reader_loop(); });
                    return true;
                }
                closesocket(s);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    }
    void stop() {
        run_ = false;
        if (sock_ != INVALID_SOCKET) { shutdown(sock_, 2); closesocket(sock_); sock_ = INVALID_SOCKET; }
        if (reader_.joinable()) reader_.join();
    }
    bool send_line(const std::string& line) {   // op:send
        std::string m = "{\"op\":\"send\",\"line\":";
        xp::json_escape_into(m, line); m += "}";
        return write_(m);
    }
    void set_deadman(const std::string& line) {
        std::string m = "{\"op\":\"set_deadman\",\"line\":";
        xp::json_escape_into(m, line); m += "}";
        write_(m);
    }
    void say_bye() { write_("{\"op\":\"bye\"}"); }
    bool up() const { return up_.load(std::memory_order_relaxed); }
    // Drain buffered PLC-originated lines, newline-joined, into buf; return bytes.
    int drain(char* buf, int buflen) {
        std::lock_guard<std::mutex> lk(in_mu_);
        int used = 0;
        while (!inbox_.empty()) {
            const std::string& l = inbox_.front();
            int need = (int)l.size() + 1;
            if (used + need > buflen) break;
            std::memcpy(buf + used, l.data(), l.size());
            used += (int)l.size();
            buf[used++] = '\n';
            inbox_.pop_front();
        }
        return used;
    }
private:
    bool write_(const std::string& msg) {
        if (sock_ == INVALID_SOCKET) return false;
        std::string out = msg; out.push_back('\n');
        std::lock_guard<std::mutex> lk(send_mu_);
        return ::send(sock_, out.data(), (int)out.size(), 0) == (int)out.size();
    }
    void reader_loop() {
        std::string buf;
        char tmp[4096];
        while (run_.load()) {
            int r = ::recv(sock_, tmp, sizeof(tmp), 0);
            if (r <= 0) { up_ = false; break; }
            buf.append(tmp, r);
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos); buf.erase(0, pos + 1);
                if (line.empty()) continue;
                cJSON* root = cJSON_Parse(line.c_str());
                if (!root) continue;
                cJSON* ev = cJSON_GetObjectItem(root, "event");
                if (cJSON_IsString(ev)) {
                    if (std::strcmp(ev->valuestring, "plc_in") == 0) {
                        if (cJSON* l = cJSON_GetObjectItem(root, "line"); cJSON_IsString(l)) {
                            std::lock_guard<std::mutex> lk(in_mu_);
                            if (inbox_.size() < 4096) inbox_.emplace_back(l->valuestring);
                        }
                    } else if (std::strcmp(ev->valuestring, "plc_up") == 0) {
                        cJSON* u = cJSON_GetObjectItem(root, "up");
                        up_ = cJSON_IsTrue(u);
                    }
                }
                cJSON_Delete(root);
            }
        }
    }
    SOCKET                   sock_ = INVALID_SOCKET;
    std::thread              reader_;
    std::atomic<bool>        run_{false};
    std::atomic<bool>        up_{false};
    std::mutex               in_mu_;
    std::deque<std::string>  inbox_;
    std::mutex               send_mu_;
};

static GatewayClient* g_gateway = nullptr;   // non-null when --comms-port is set

static int  comms_send_cb(const char* line) { return (g_gateway && g_gateway->send_line(line ? line : "")) ? 1 : 0; }
static int  comms_poll_cb(char* buf, int n) { return g_gateway ? g_gateway->drain(buf, n) : 0; }
static int  comms_up_cb()                   { return (g_gateway && g_gateway->up()) ? 1 : 0; }
static void comms_deadman_cb(const char* line) { if (g_gateway) g_gateway->set_deadman(line ? line : ""); }

// Forward-declare: runs one inspection cycle and emits vars+previews.
// If run_id == 0, auto-generates one. frame_hint is passed to inspect().
// frame_path (optional) is plumbed to the script via
// `xi_script_set_run_context`; readable inside the script as
// `xi::current_frame_path()`. Empty string means none.
static void run_one_inspection(xi::ws::Server& srv,
                               int frame_hint = 1,
                               int64_t run_id = 0,
                               const std::string& frame_path = "",
                               int64_t emit_seq = -1,
                               EmitGate* gate = &g_global_gate);

// Path resolution for the script compiler. Backend derives its own dir at
// startup and uses that to locate the xi headers we ship alongside the exe.
static std::string g_include_dir;
static std::string g_work_dir;
static std::string g_plugins_dir;
// Accelerator install roots, probed once at startup. Empty string =
// not installed → user scripts fall back to portable C++ for that path.
static std::string g_opencv_dir;
static std::string g_turbojpeg_root;
static std::string g_ipp_root;
// vcvars64.bat override (empty = let the compiler auto-find via auto_find_vcvars).
static std::string g_tc_vcvars;
// Canonical folder of the currently-open project (the one the user edits — NOT
// any .xinsp_work scratch). Set on open_project; used to read/write the
// per-project "toolchain" override block in its project.json.
static std::string g_project_folder;
// The xi include dir derived from the exe location at startup. Kept separate from
// g_include_dir so a project override can point elsewhere yet we can always fall
// back to the shipped headers.
static std::string g_include_dir_default;

// IntelliSense config generation. The C/C++ extension (Microsoft) has no way to
// know our compile flags — inspect.cpp / plugin .cpp are compiled by the backend,
// not by CMake — so #include <xi/xi.hpp> and <opencv2/...> light up red and
// go-to-definition fails. We fix that by writing a c_cpp_properties.json into the
// opened project that mirrors the SAME include set + std + force-include the
// compiler actually uses (see CompileRequest wiring at ~:1632). The backend owns
// the real paths (g_include_dir / g_opencv_dir / ...), so it's the right place to
// emit this — no path guessing in the extension.
//
// TODO(linux): the config is MSVC-flavoured (intelliSenseMode windows-msvc-x64,
// _WIN32). When the Linux port lands, emit linux-clang-x64 + the gcc/clang std lib
// paths instead; gate on the host like the rest of the toolchain probing.
static void write_cpp_intellisense_config_(const std::string& project_folder) {
    namespace fs = std::filesystem;
    if (g_include_dir.empty()) return;  // nothing useful to point at
    std::error_code ec;
    fs::path vsdir = fs::path(project_folder) / ".vscode";
    fs::path cfg   = vsdir / "c_cpp_properties.json";

    // Never clobber a hand-written config. We only own files we stamped.
    if (fs::exists(cfg, ec)) {
        std::ifstream in(cfg.string());
        std::stringstream ss; ss << in.rdbuf();
        if (ss.str().find("\"_generated_by\": \"xinsp2\"") == std::string::npos) {
            std::fprintf(stderr, "[xinsp2] c_cpp_properties.json exists and is "
                                 "user-owned; leaving it untouched\n");
            return;
        }
    }
    fs::create_directories(vsdir, ec);

    // VS Code accepts forward slashes on Windows, so normalise and skip JSON
    // backslash-escaping entirely.
    auto fwd = [](std::string p) { for (auto& c : p) if (c == '\\') c = '/'; return p; };

    std::vector<std::string> inc;
    inc.push_back(fwd(g_include_dir));
    {   // vendor sits beside include in the shipped layout (include/.. /vendor)
        fs::path vendor = fs::path(g_include_dir).parent_path() / "vendor";
        inc.push_back(fwd(vendor.string()));
    }
    if (!g_opencv_dir.empty())     inc.push_back(fwd(g_opencv_dir) + "/include");
    if (!g_turbojpeg_root.empty()) inc.push_back(fwd(g_turbojpeg_root) + "/include");
    if (!g_ipp_root.empty())       inc.push_back(fwd(g_ipp_root) + "/include");
    inc.push_back("${workspaceFolder}/**");

    std::string defs = "\"_WIN32\", \"_WIN64\"";
    if (!g_turbojpeg_root.empty()) defs += ", \"XINSP2_HAS_TURBOJPEG=1\"";

    // Force-include the script support header so VAR()/EMIT()/XI_SCRIPT_EXPORT
    // resolve in inspect.cpp — those are macros the compiler force-includes, not
    // something the user #includes. Plugin .cpp pick up extra (unused) script
    // symbols from it; harmless for IntelliSense parsing.
    std::string force_inc = fwd(g_include_dir) + "/xi/xi_script_support.hpp";

    std::string body;
    body += "{\n";
    body += "  \"_generated_by\": \"xinsp2\",\n";
    body += "  \"_note\": \"Auto-generated by the xInsp2 backend on open_project. "
            "Mirrors the compiler's include set so IntelliSense resolves xi/* and "
            "OpenCV. Delete _generated_by to take manual ownership.\",\n";
    body += "  \"version\": 4,\n";
    body += "  \"configurations\": [\n";
    body += "    {\n";
    body += "      \"name\": \"xInsp2\",\n";
    body += "      \"includePath\": [\n";
    for (size_t i = 0; i < inc.size(); ++i)
        body += "        \"" + inc[i] + "\"" + (i + 1 < inc.size() ? ",\n" : "\n");
    body += "      ],\n";
    body += "      \"forcedInclude\": [\"" + force_inc + "\"],\n";
    body += "      \"defines\": [" + defs + "],\n";
    body += "      \"cStandard\": \"c17\",\n";
    body += "      \"cppStandard\": \"c++20\",\n";
    body += "      \"intelliSenseMode\": \"windows-msvc-x64\"\n";
    body += "    }\n";
    body += "  ]\n";
    body += "}\n";

    std::ofstream out(cfg.string(), std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "[xinsp2] could not write %s\n", cfg.string().c_str());
        return;
    }
    out << body;
    std::fprintf(stderr, "[xinsp2] wrote IntelliSense config -> %s\n", cfg.string().c_str());

    // c_cpp_properties.json only does anything if the Microsoft C/C++ extension
    // is installed. Recommend it (gentle "install recommended extensions?" prompt,
    // never a forced install) the first time — but only if there's no
    // extensions.json yet, so we don't stomp a workspace's own recommendation set.
    fs::path ext = vsdir / "extensions.json";
    if (!fs::exists(ext, ec)) {
        std::ofstream eo(ext.string(), std::ios::binary | std::ios::trunc);
        if (eo) eo << "{\n  \"recommendations\": [\"ms-vscode.cpptools\"]\n}\n";
    }
}

// ---- C++ toolchain health + per-project override -----------------------------
//
// A project may pin toolchain paths in its project.json "toolchain" block:
//   "toolchain": {
//     "include_dir":     "...",   // xi headers (defaults to the shipped set)
//     "opencv_dir":      "...",   // OpenCV install root
//     "turbojpeg_root":  "...",   // libjpeg-turbo root (optional accelerator)
//     "ipp_root":        "...",   // Intel IPP root      (optional accelerator)
//     "vcvars":          "..."    // path to vcvars64.bat (else auto-found)
//   }
// Resolution priority per component: project override > env var > built-in probe
// (which itself checks env then default candidates). This lets a user fix a
// wrong/missing path from the VS Code config UI without touching global
// environment. The same resolved values feed BOTH the compiler and the
// generated c_cpp_properties.json, so IntelliSense can never drift from the build.
struct TcComponent {
    std::string key;       // stable id: "include" | "opencv" | "turbojpeg" | "ipp" | "vcvars"
    std::string label;     // human label
    std::string ov_key;    // project.json toolchain field name
    std::string env_var;   // env var that also sets it ("" = none)
    std::string sentinel;  // relative file proving the dir is real ("" = path is a file)
    std::string path;      // resolved path (may be empty)
    std::string source;    // "override" | "env" | "default" | "none"
    bool exists = false;   // sentinel (or the file itself, for vcvars) present
    bool optional = false; // optional accelerator → missing is info, not error
};

// Read one string field from the "toolchain" object of <folder>/project.json.
static std::string read_toolchain_override_(const std::string& folder, const char* field) {
    if (folder.empty()) return {};
    std::ifstream in((std::filesystem::path(folder) / "project.json").string());
    if (!in) return {};
    std::stringstream ss; ss << in.rdbuf();
    std::string out;
    if (cJSON* root = cJSON_Parse(ss.str().c_str())) {
        if (cJSON* tc = cJSON_GetObjectItem(root, "toolchain"); tc && cJSON_IsObject(tc))
            if (cJSON* k = cJSON_GetObjectItem(tc, field); k && cJSON_IsString(k) && k->valuestring)
                out = k->valuestring;
        cJSON_Delete(root);
    }
    return out;
}

static bool tc_sentinel_ok_(const TcComponent& c) {
    if (c.path.empty()) return false;
    std::error_code ec;
    if (c.sentinel.empty())  // vcvars: the path IS the file
        return std::filesystem::exists(c.path, ec);
    return std::filesystem::exists(std::filesystem::path(c.path) / c.sentinel, ec);
}

// Build the live component list for `folder` (the open project; "" = no project,
// startup defaults only). `path` comes straight from override/probe so it's always
// accurate; `source` is best-effort labelling for the UI.
static std::vector<TcComponent> resolve_toolchain_components_(const std::string& folder) {
    using namespace xi::script::detail;
    std::vector<TcComponent> v(5);
    v[0] = { "include",   "xi headers",        "include_dir",    "",               "xi/xi.hpp",                  "", "", false, false };
    v[1] = { "opencv",    "OpenCV",            "opencv_dir",     "OpenCV_DIR",     "include/opencv2/core.hpp",   "", "", false, false };
    v[2] = { "turbojpeg", "libjpeg-turbo",     "turbojpeg_root", "TURBOJPEG_ROOT", "include/turbojpeg.h",        "", "", false, true  };
    v[3] = { "ipp",       "Intel IPP",         "ipp_root",       "IPP_ROOT",       "include/ippi.h",             "", "", false, true  };
    v[4] = { "vcvars",    "MSVC (vcvars64)",   "vcvars",         "",               "",                           "", "", false, false };

    for (auto& c : v) {
        std::string ov = read_toolchain_override_(folder, c.ov_key.c_str());
        if (!ov.empty()) { c.path = ov; c.source = "override"; }
        else {
            // Built-in probe (already honours env then default candidates).
            std::string probed;
            if      (c.key == "include")   probed = g_include_dir_default;
            else if (c.key == "opencv")    probed = probe_opencv_dir();
            else if (c.key == "turbojpeg") probed = probe_turbojpeg_root();
            else if (c.key == "ipp")       probed = probe_ipp_root();
            else if (c.key == "vcvars")    probed = auto_find_vcvars();
            c.path = probed;
            const char* e = c.env_var.empty() ? nullptr : std::getenv(c.env_var.c_str());
            if (!c.path.empty() && e && *e)   c.source = "env";
            else if (!c.path.empty())         c.source = "default";
            else                              c.source = "none";
        }
        c.exists = tc_sentinel_ok_(c);
    }
    return v;
}

// Apply a project's toolchain resolution to the global compiler paths. Called on
// open_project and after set_toolchain_override so the next compile + the
// generated IntelliSense config both pick up the override immediately.
static void resolve_toolchain_(const std::string& folder) {
    auto comps = resolve_toolchain_components_(folder);
    for (auto& c : comps) {
        if      (c.key == "include")   { if (c.source == "override") g_include_dir = c.path; else g_include_dir = g_include_dir_default; }
        else if (c.key == "opencv")    g_opencv_dir     = c.path;
        else if (c.key == "turbojpeg") g_turbojpeg_root = c.path;
        else if (c.key == "ipp")       g_ipp_root       = c.path;
        else if (c.key == "vcvars")    g_tc_vcvars      = (c.source == "override") ? c.path : std::string();
    }
    std::fprintf(stderr, "[xinsp2] toolchain resolved: opencv=%s turbojpeg=%s ipp=%s vcvars=%s\n",
                 g_opencv_dir.empty() ? "none" : g_opencv_dir.c_str(),
                 g_turbojpeg_root.empty() ? "none" : g_turbojpeg_root.c_str(),
                 g_ipp_root.empty() ? "none" : g_ipp_root.c_str(),
                 g_tc_vcvars.empty() ? "auto" : g_tc_vcvars.c_str());
}

// Render the health report as JSON for the toolchain_health command / UI.
static std::string toolchain_health_json_(const std::string& folder) {
    auto comps = resolve_toolchain_components_(folder);
    bool all_ok = true;
    std::string out = "{\"components\":[";
    for (size_t i = 0; i < comps.size(); ++i) {
        auto& c = comps[i];
        // ok rules: an explicit override that doesn't resolve is always an error
        // (the user pointed us somewhere wrong); a required component must exist;
        // an optional one that's simply absent is fine.
        bool ok;
        if (c.source == "override") ok = c.exists;
        else if (!c.optional)       ok = c.exists;
        else                        ok = true;
        if (!ok) all_ok = false;

        std::string hint;
        if (c.source == "override" && !c.exists)
            hint = c.sentinel.empty() ? "overridden path does not exist"
                                      : ("expected " + c.sentinel + " under this folder");
        else if (!c.exists && !c.optional)
            hint = c.key == "vcvars" ? "vcvars64.bat not found — install VS Build Tools (Desktop C++ workload)"
                                     : ("not found — set " + (c.env_var.empty() ? std::string("an override") : c.env_var) + " or fix the path");
        else if (!c.exists && c.optional)
            hint = "optional accelerator, not installed";

        if (i) out += ",";
        out += "{\"key\":";       xp::json_escape_into(out, c.key);
        out += ",\"label\":";     xp::json_escape_into(out, c.label);
        out += ",\"path\":";      xp::json_escape_into(out, c.path);
        out += ",\"source\":";    xp::json_escape_into(out, c.source);
        out += ",\"env_var\":";   xp::json_escape_into(out, c.env_var);
        out += ",\"ov_key\":";    xp::json_escape_into(out, c.ov_key);
        out += ",\"exists\":";    out += c.exists ? "true" : "false";
        out += ",\"optional\":";  out += c.optional ? "true" : "false";
        out += ",\"ok\":";        out += ok ? "true" : "false";
        out += ",\"hint\":";      xp::json_escape_into(out, hint);
        out += "}";
    }
    out += "],\"all_ok\":";
    out += all_ok ? "true" : "false";
    out += ",\"project\":";
    xp::json_escape_into(out, folder);
    out += "}";
    return out;
}

// Merge one override into the canonical project.json "toolchain" block. Empty
// value clears that key (revert to env/probe). Returns false (with `err`) if the
// project.json can't be read/parsed/written.
static bool write_toolchain_override_(const std::string& folder, const std::string& field,
                                      const std::string& value, std::string& err) {
    namespace fs = std::filesystem;
    if (folder.empty()) { err = "no project open"; return false; }
    fs::path pj = fs::path(folder) / "project.json";
    std::ifstream in(pj.string());
    if (!in) { err = "cannot read project.json"; return false; }
    std::stringstream ss; ss << in.rdbuf();
    in.close();
    cJSON* root = cJSON_Parse(ss.str().c_str());
    if (!root) { err = "project.json is not valid JSON"; return false; }
    cJSON* tc = cJSON_GetObjectItem(root, "toolchain");
    if (!tc || !cJSON_IsObject(tc)) {
        cJSON_DeleteItemFromObject(root, "toolchain");  // drop any non-object
        tc = cJSON_AddObjectToObject(root, "toolchain");
    }
    if (value.empty()) cJSON_DeleteItemFromObject(tc, field.c_str());
    else {
        cJSON_DeleteItemFromObject(tc, field.c_str());
        cJSON_AddStringToObject(tc, field.c_str(), value.c_str());
    }
    // Drop an emptied toolchain object so we don't leave "toolchain":{} noise.
    if (cJSON_GetArraySize(tc) == 0) cJSON_DeleteItemFromObject(root, "toolchain");
    char* printed = cJSON_Print(root);
    bool ok = false;
    if (printed) {
        std::ofstream o(pj.string(), std::ios::binary | std::ios::trunc);
        if (o) { o << printed << "\n"; ok = true; }
        else err = "cannot write project.json";
        cJSON_free(printed);
    } else err = "failed to serialize project.json";
    cJSON_Delete(root);
    return ok;
}

// ---- script external dependencies (project.json include_dirs / link_libs) ----
//
// A user script (inspect.cpp) may need an external SDK. Unlike a plugin (which
// ships deps in its own folder), the script DLL lives in TEMP/script_build, so we
// give the script two project-level hooks:
//   "include_dirs": ["deps/include", "C:/abs/include"]  -> extra cl /I
//   "link_libs":    ["deps/foo.lib"]                     -> import libs to link
// Relative entries resolve against the project folder. The matching runtime DLL
// search of the project folder is set up in set_project_dll_search_ (below).
static void read_script_deps_(const std::string& folder,
                              std::vector<std::string>& include_dirs,
                              std::vector<std::string>& link_libs) {
    if (folder.empty()) return;
    namespace fs = std::filesystem;
    std::ifstream in((fs::path(folder) / "project.json").string());
    if (!in) return;
    std::stringstream ss; ss << in.rdbuf();
    cJSON* root = cJSON_Parse(ss.str().c_str());
    if (!root) return;
    auto resolve = [&](const char* s) -> std::string {
        fs::path p(s);
        if (p.is_absolute()) return p.string();
        std::error_code ec;
        return (fs::path(folder) / p).lexically_normal().string();
    };
    auto pull = [&](const char* key, std::vector<std::string>& out) {
        cJSON* arr = cJSON_GetObjectItem(root, key);
        if (!arr || !cJSON_IsArray(arr)) return;
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, arr)
            if (cJSON_IsString(it) && it->valuestring && *it->valuestring)
                out.push_back(resolve(it->valuestring));
    };
    pull("include_dirs", include_dirs);
    pull("link_libs", link_libs);
    cJSON_Delete(root);
}

// Put the open project's folder on the process DLL search path so a script's
// statically-linked external dependency DLL can live in the project folder. The
// script loader (xi_script_loader.hpp) loads with LOAD_LIBRARY_SEARCH_USER_DIRS,
// which honours dirs added via AddDllDirectory. Re-pointed on each open_project.
// TODO(linux): equivalent is building the script .so with -Wl,-rpath plus
// dlopen; AddDllDirectory has no portable analogue.
static DLL_DIRECTORY_COOKIE g_proj_dll_dir = nullptr;
static void set_project_dll_search_(const std::string& folder) {
    if (g_proj_dll_dir) { RemoveDllDirectory(g_proj_dll_dir); g_proj_dll_dir = nullptr; }
    if (folder.empty()) return;
    int wn = MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), -1, nullptr, 0);
    if (wn <= 0) return;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), -1, w.data(), wn);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    g_proj_dll_dir = AddDllDirectory(w.c_str());
}

// Plugin manager (global)
static xi::PluginManager g_plugin_mgr;

static std::string get_exe_dir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::filesystem::path p(std::string(buf, n));
    return p.parent_path().string();
}


static std::atomic<bool> g_should_exit{false};

static int parse_port(int argc, char** argv) {
    int port = 7823;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind("--port=", 0) == 0) {
            try { port = std::stoi(std::string(a.substr(7))); } catch (...) {}
        } else if (a == "--port" && i + 1 < argc) {
            try { port = std::stoi(argv[++i]); } catch (...) {}
        }
    }
    return port;
}

// --host=<addr>  (default 127.0.0.1). Pass 0.0.0.0 for remote-reachable.
static std::string parse_host(int argc, char** argv) {
    std::string host = "127.0.0.1";
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind("--host=", 0) == 0) host = std::string(a.substr(7));
        else if (a == "--host" && i + 1 < argc) host = argv[++i];
    }
    if (const char* env = std::getenv("XINSP2_HOST"); env && *env) host = env;
    return host;
}

// --watchdog=<ms>  (default 0 = disabled). When non-zero, every inspect()
// call has this many ms of wall-clock budget before the watchdog
// terminates the runaway thread.
static int parse_watchdog_ms(int argc, char** argv) {
    int ms = 0;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind("--watchdog=", 0) == 0) { try { ms = std::stoi(std::string(a.substr(11))); } catch (...) {} }
        else if (a == "--watchdog" && i + 1 < argc) { try { ms = std::stoi(argv[++i]); } catch (...) {} }
    }
    if (ms < 0) ms = 0;
    if (ms > 600000) ms = 600000;
    return ms;
}

// --auth=<secret>  (default empty = no auth required).
// Also XINSP2_AUTH env var (preferred on shared servers — no argv leak to ps).
static std::string parse_auth_secret(int argc, char** argv) {
    std::string secret;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind("--auth=", 0) == 0) secret = std::string(a.substr(7));
        else if (a == "--auth" && i + 1 < argc) secret = argv[++i];
    }
    if (const char* env = std::getenv("XINSP2_AUTH"); env && *env) secret = env;
    return secret;
}

// --project=<dir> / --script=<path> : headless autostart. When --project is set,
// main() drives open_project -> compile_and_load -> (optional) start at boot, so
// the backend runs a line without any WS client (the xinsp-fe supervisor only
// manages the process). Returns empty if the flag is absent.
static std::string parse_str_flag(int argc, char** argv, const char* flag) {
    std::string eq = std::string(flag) + "=";
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind(eq, 0) == 0) return std::string(a.substr(eq.size()));
        if (a == flag && i + 1 < argc) return argv[i + 1];
    }
    return {};
}

// Presence check for a bare flag (e.g. --hang-before-ready).
static bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == flag) return true;
    return false;
}

// --autostart-fps=<N>  (default 0 = don't auto-start continuous mode; just
// open+compile and wait for a client / triggers). N < 0 = autostart in
// TRIGGER-ONLY mode (continuous on, lanes spawned, no synthetic timer tick —
// the project's sources drive everything).
static int parse_autostart_fps(int argc, char** argv) {
    std::string v = parse_str_flag(argc, argv, "--autostart-fps");
    if (v.empty()) return 0;
    try { return std::stoi(v); } catch (...) { return 0; }
}

// Repeatable: --plugins-dir=/some/path  (or --plugins-dir /some/path).
// Also reads XINSP2_EXTRA_PLUGIN_DIRS, semicolon- or path-separator-delimited.
static std::vector<std::string> parse_extra_plugin_dirs(int argc, char** argv) {
    std::vector<std::string> dirs;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind("--plugins-dir=", 0) == 0) {
            dirs.emplace_back(std::string(a.substr(14)));
        } else if (a == "--plugins-dir" && i + 1 < argc) {
            dirs.emplace_back(argv[++i]);
        }
    }
    if (const char* env = std::getenv("XINSP2_EXTRA_PLUGIN_DIRS")) {
        std::string s(env);
        size_t start = 0;
        while (start <= s.size()) {
            size_t end = s.find_first_of(";,", start);
            if (end == std::string::npos) end = s.size();
            std::string item = s.substr(start, end - start);
            if (!item.empty()) dirs.push_back(std::move(item));
            if (end == s.size()) break;
            start = end + 1;
        }
    }
    return dirs;
}

static double now_seconds() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

static void send_rsp_ok(xi::ws::Server& srv, int64_t id, std::string data_json = "") {
    xp::Rsp r;
    r.id = id;
    r.ok = true;
    r.data_json = std::move(data_json);
    srv.send_text(r.to_json());
}

// Ring buffer of recent error messages so an AI / scripted client can
// correlate a synchronous cmd with any side-channel errors that might
// have raced in (run-thread crashes, log-level=error from background
// activity, isolation_dead events, etc). Three error channels exist
// in the protocol — rsp.error (sync), `event` (async), `log`
// level=error (async) — and the WS spec doesn't carry cmd_id /
// run_id on the async two. Until that's fixed protocol-wide, this
// ring lets the client pull "anything error-shaped that happened
// in the last minute" with a single query.
struct RecentError {
    int64_t     ts_ms = 0;
    std::string source;     // "rsp" / "log" / "event"
    std::string message;
    int64_t     cmd_id  = 0;   // 0 if unknown
    int64_t     run_id  = 0;   // 0 if unknown
};
static std::mutex                     g_recent_errors_mu;
static std::deque<RecentError>        g_recent_errors;
static constexpr size_t               kRecentErrorsCap = 64;

static int64_t now_ms_() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static void push_recent_error(std::string source, std::string message,
                              int64_t cmd_id = 0, int64_t run_id = 0) {
    RecentError e{ now_ms_(), std::move(source), std::move(message), cmd_id, run_id };
    std::lock_guard<std::mutex> lk(g_recent_errors_mu);
    g_recent_errors.push_back(std::move(e));
    while (g_recent_errors.size() > kRecentErrorsCap) g_recent_errors.pop_front();
}

static void send_rsp_err(xi::ws::Server& srv, int64_t id, std::string err) {
    xp::Rsp r;
    r.id = id;
    r.ok = false;
    r.error = err;
    srv.send_text(r.to_json());
    push_recent_error("rsp", std::move(err), id);
}

// Send a log {level:error, msg:...} AND record it in the recent-error
// ring so cmd:recent_errors can surface it. Most error logs go
// through this; a few legacy sites still build the LogMsg inline —
// migrating them to this helper is mechanical and ongoing.
static void emit_error_log(xi::ws::Server& srv, const std::string& msg,
                           int64_t run_id = 0) {
    xp::LogMsg lm; lm.level = "error"; lm.msg = msg;
    srv.send_text(lm.to_json());
    push_recent_error("log", msg, /*cmd_id=*/0, run_id);
}

static void send_hello(xi::ws::Server& srv) {
    xp::Event e;
    e.name = "hello";
    e.data_json = std::string(R"({"version":")") + XINSP2_VERSION
                + R"(","commit":")" + XINSP2_COMMIT
                + R"(","abi":1})";
    srv.send_text(e.to_json());
}

// Shared function: emit vars + binary preview frames from the script's
// thunks or the built-in demo. Called by `cmd: run` and by the continuous
// trigger loop.
static void emit_vars_and_previews(xi::ws::Server& srv,
                                    xi::script::LoadedScript& s,
                                    int64_t run_id, int64_t dt_ms) {
    if (s.ok() && s.snapshot) {
        // Script path — read from DLL thunks
        std::vector<char> sbuf(256 * 1024);
        int n = s.snapshot(sbuf.data(), (int)sbuf.size());
        if (n < 0) { sbuf.resize((size_t)(-(int64_t)n) + 1024);
                     n = s.snapshot(sbuf.data(), (int)sbuf.size()); }
        if (n <= 0) return;

        // vars text message
        std::string vars_msg = "{\"type\":\"vars\",\"run_id\":";
        vars_msg += std::to_string(run_id);
        vars_msg += ",\"items\":";
        vars_msg += std::string(sbuf.data(), (size_t)n);
        vars_msg += "}";
        srv.send_text(vars_msg);

        // S4: stash this run's vars snapshot in the history ring so a
        // client can scrub backward through recent runs without re-running.
        {
            std::lock_guard<std::mutex> lk(g_hist_mu);
            int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            g_history.push_back({ run_id, ts_ms, std::string(sbuf.data(), (size_t)n) });
            while (g_history.size() > g_hist_max) g_history.pop_front();
        }

        // image previews — filtered by subscription. For each
        // `"name":"X"` followed by `"gid":N`, only emit the JPEG when
        // the subscription set allows it (or we're in send-all mode).
        bool sub_all;
        std::unordered_set<std::string> sub_names;
        {
            std::lock_guard<std::mutex> lk(g_sub_mu);
            sub_all = g_sub_all;
            if (!sub_all) sub_names = g_sub_names;   // copy under lock
        }

        std::string_view snap_view(sbuf.data(), (size_t)n);
        size_t pos = 0;
        std::string cur_name;
        while (pos < snap_view.size()) {
            // Track the latest `"name":"..."` we saw; every later `"gid":`
            // is assumed to belong to that entry (snapshot emits name
            // before gid within each item).
            auto nm = snap_view.find("\"name\":\"", pos);
            auto gd = snap_view.find("\"gid\":", pos);
            if (gd == std::string_view::npos) break;
            if (nm != std::string_view::npos && nm < gd) {
                nm += 8;
                auto end = snap_view.find('"', nm);
                if (end != std::string_view::npos) cur_name = std::string(snap_view.substr(nm, end - nm));
            }
            pos = gd + 6;
            uint32_t gid = (uint32_t)std::atoi(snap_view.data() + pos);
            if (!sub_all && !sub_names.count(cur_name)) continue;
            if (s.dump_image) {
                // Buffers are thread_local + reused across calls. Without
                // this, every preview allocated 32 MB of raw + a fresh
                // JPEG vector + a fresh frame vector PER IMAGE PER FRAME
                // — at 30 fps × 4 images = 3.8 GB/s of allocator churn,
                // which dominated the encode time and tail-latency-spiked
                // the malloc heap. Reuse + size-on-demand keeps the
                // resident set bounded by the largest image seen so far.
                static thread_local std::vector<uint8_t> raw;
                static thread_local std::vector<uint8_t> jpeg;
                static thread_local std::vector<uint8_t> frame;
                int w = 0, h = 0, c = 0;
                // First call asks for size via the convention
                // (negative return = need that much). dump_image still
                // wants a real buffer — start at 1 MB and grow.
                if (raw.size() < 1 * 1024 * 1024) raw.resize(1 * 1024 * 1024);
                int nb = s.dump_image(gid, raw.data(), (int)raw.size(), &w, &h, &c);
                if (nb < 0) {
                    raw.resize((size_t)(-nb) + 1024);
                    nb = s.dump_image(gid, raw.data(), (int)raw.size(), &w, &h, &c);
                }
                if (nb > 0 && w > 0 && h > 0 && c > 0) {
                    xi::Image img(w, h, c, raw.data());
                    jpeg.clear();
                    if (xi::encode_jpeg(img, 85, jpeg)) {
                        size_t total = xp::kPreviewHeaderSize + jpeg.size();
                        if (frame.size() < total) frame.resize(total);
                        xp::PreviewHeader hd;
                        hd.gid = gid; hd.codec = (uint32_t)xp::Codec::JPEG;
                        hd.width = (uint32_t)w; hd.height = (uint32_t)h; hd.channels = (uint32_t)c;
                        xp::encode_preview_header(hd, frame.data());
                        std::memcpy(frame.data() + xp::kPreviewHeaderSize, jpeg.data(), jpeg.size());
                        srv.send_binary(frame.data(), total);
                    }
                }
            }
        }
    }
}

// (seh_exception and seh_translator defined above, before use_process_cb)

// Run one full inspection cycle: reset → inspect → emit.
// The inspect call is wrapped in SEH so a script crash (null deref,
// divide-by-zero, stack overflow) is caught without killing the backend.
static void run_one_inspection(xi::ws::Server& srv, int frame_hint,
                               int64_t run_id, const std::string& frame_path,
                               int64_t emit_seq, EmitGate* gate) {
    if (run_id == 0) run_id = ++g_run_id;

    xi::script::LoadedScript s;
    {
        std::lock_guard<std::mutex> lk(g_script_mu);
        s = g_script;
    }

    if (!s.ok()) {
        xp::LogMsg lm;
        lm.level = "warn";
        lm.msg = "no script loaded — compile a .cpp first";
        srv.send_text(lm.to_json());
        return;
    }

    // Plumb the optional per-run context (frame_path) into the script
    // DLL's globals before inspect runs. Cleared on the way out so a
    // subsequent run with no frame_path arg sees an empty string,
    // not the previous value.
    if (s.set_run_context) s.set_run_context(frame_path.c_str());

    // Per-run Result: reset to NA before the script runs, and snapshot the
    // source/group provenance from this thread's trigger (thread_local, valid
    // for the duration of the inspect). The script sets the result via
    // xi::result() → result_cb → g_run_result; we emit it below in the gate.
    g_run_result = RunResult{};
    std::string rr_source, rr_group;
    if (g_current_trigger) { rr_source = g_current_trigger->leader_source; rr_group = g_current_trigger->group; }

    // F-P1-1: bracket the inspect with run_started / run_finished /
    // run_error events so SDK callers can observe lifecycle outside the
    // synchronous rsp path. Documented in docs/protocol.md.
    auto emit_run_event = [&srv, run_id](const char* name,
                                          const std::string& extra_data = "") {
        xp::Event ev;
        ev.name = name;
        std::string data = "{\"run_id\":" + std::to_string(run_id);
        if (!extra_data.empty()) { data += ","; data += extra_data; }
        data += "}";
        ev.data_json = data;
        srv.send_text(ev.to_json());
    };
    emit_run_event("run_started");

    auto t0 = std::chrono::steady_clock::now();
    // Arm the watchdog: claim a per-inspect slot holding this inspect's
    // deadline. Works for any dispatch_threads (N slots), unlike the old
    // single-slot scheme that had to skip N>1. Cleared in the post-inspect
    // path below regardless of throw. No thread handle is kept — a hard trip
    // exits the process (FE respawns) rather than TerminateThread'ing a worker
    // (which would leak the per-instance lock + risk heap corruption).
    int wd_slot = -1;
    int wd_ms = g_watchdog_ms.load();
    if (wd_ms > 0) {
        // D-P1-10: deadline must use steady_clock (monotonic) — a system_clock
        // NTP/DST jump would skip every deadline or hang the watchdog forever.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wd_ms);
        wd_slot = wd_arm(std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline.time_since_epoch()).count());
    }
    auto disarm = [&]() { wd_disarm(wd_slot); wd_slot = -1; };
    // Stamp this dispatch thread's crash breadcrumb so a fault inside
    // the inspect (the most common crash site) names the run + phase
    // for THIS thread, not whatever the last thread to touch the
    // global wrote. frame_hint doubles as the per-thread frame marker.
    {
        auto& c = crash_ctx();
        c.last_run_id = (int)run_id;
        c.last_frame  = frame_hint;
        crash_set(c.last_cmd, sizeof(c.last_cmd), "inspect");
    }
    // Run the inspect (in parallel under N>1). Capture success/error WITHOUT
    // emitting yet — emission happens below under the EmitTurn gate so ordered
    // mode (parallelism.result_order: "arrival") can serialize the wire stream
    // by frame-arrival order. Logs (diagnostic) stay immediate; the run_error
    // EVENT is deferred so it's ordered with run_finished and the turn always
    // advances (an error must not stall the ordered stream).
    bool inspect_ok = false;
    std::string run_error_what;   // set on failure; empty on success
    try {
        // Tag any image_create calls inside the script's inspect (and
        // any plugin process_fn it transitively calls that didn't set
        // its own guard) with the script's owner_id. Per-script
        // sweep-on-unload + per-instance sweep-on-destroy together
        // catch the leaked-handle case from both directions.
        xi::ImagePool::OwnerGuard sg(s.owner_id);
        crash_set_phase("reset");
        if (s.reset) s.reset();
        crash_set_phase("inspect");
        s.inspect(frame_hint);
        crash_set_phase("done");
        disarm();
        inspect_ok = true;
    } catch (const seh_exception& e) {
        disarm();
        auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0).count();
        char msg[256];
        std::snprintf(msg, sizeof(msg), "script crashed after %lldms: 0x%08X (%s)",
                     (long long)dt_ms, e.code, e.what());
        std::fprintf(stderr, "[xinsp2] %s\n", msg);
        emit_error_log(srv, msg, run_id);
        run_error_what = "\"what\":";
        xp::json_escape_into(run_error_what, std::string(msg));
    } catch (const std::exception& e) {
        disarm();
        std::fprintf(stderr, "[xinsp2] inspect threw: %s\n", e.what());
        emit_error_log(srv, std::string("script exception: ") + e.what(), run_id);
        run_error_what = "\"what\":";
        xp::json_escape_into(run_error_what, std::string("script exception: ") + e.what());
    } catch (...) {
        disarm();
        run_error_what = "\"what\":\"unknown_exception\"";
    }

    auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0).count();
    {
        // Ordered mode: block until it's this frame's turn to emit. The dtor
        // advances the cursor + wakes the next worker even if we throw here, so
        // the stream can't deadlock. No-op for emit_seq < 0 (completion mode).
        EmitTurn turn(gate, emit_seq);
        if (inspect_ok) {
            emit_vars_and_previews(srv, s, run_id, dt_ms);
            // One Result per run: whatever the script set (default 0 = NA if it
            // called no xi::result), emitted before run_finished so consumers
            // can pair them. Ordered with the rest of the stream by the gate.
            emit_run_result(srv, g_run_result.code, g_run_result.msg,
                            run_id, dt_ms, rr_source, rr_group);
            emit_run_event("run_finished",
                           "\"ms\":" + std::to_string((long long)dt_ms));
            // Clear so the next run, if it doesn't carry a frame_path arg,
            // sees an empty path instead of the stale previous one.
            if (s.set_run_context) s.set_run_context("");
        } else {
            // Inspect failed (crash/throw) — still emit one Result so the stream
            // has no gap. v1: NA (0). v1.1 upgrades this to XI_SYS_CRASHED/TIMEOUT.
            emit_run_result(srv, 0, "inspect error", run_id, dt_ms, rr_source, rr_group);
            emit_run_event("run_error", run_error_what);
        }
    }
}

// (trigger_worker removed — continuous mode uses a simple timer thread)

// Counters for queue overflow logging — incremented by enqueue_event_
// when an event has to be dropped because the dispatch queue is full.
// Logged via the WS log channel periodically so callers can see
// pressure without scraping stderr.
static std::atomic<uint64_t> g_dropped_oldest{0};
static std::atomic<uint64_t> g_dropped_newest{0};
// Observed peak queue depth since cmd:start. Useful for tuning —
// sweep1 with N=1 / queue=32 might pin at 32; sweep2 with N=4 might
// peak at 3. Reset on cmd:start.
static std::atomic<uint64_t> g_queue_high_watermark{0};

// Apply the project's queue policy and push (or drop) the event. Caller
// owns the event by value. Caller must NOT hold g_ev_mu — this fn
// takes it. Returns true if pushed, false if dropped or rejected.
//
// queue_depth and overflow read once per call from the project info
// (cheap atomics not worth it; they only change on open_project).
static bool enqueue_event_(xi::TriggerEvent ev) {
    int depth = g_plugin_mgr.project().queue_depth;
    if (depth < 1) depth = 1;
    const std::string& overflow = g_plugin_mgr.project().overflow;

    std::unique_lock<std::mutex> lk(g_ev_mu);
    if ((int)g_ev_queue.size() < depth) {
        g_ev_queue.push_back(std::move(ev));
        // Update high watermark (post-push depth).
        uint64_t now_size = g_ev_queue.size();
        uint64_t prev = g_queue_high_watermark.load(std::memory_order_relaxed);
        while (now_size > prev &&
               !g_queue_high_watermark.compare_exchange_weak(prev, now_size,
                                                              std::memory_order_relaxed)) {}
        g_ev_cv.notify_one();
        return true;
    }
    // Queue full.
    if (overflow == "drop_newest") {
        ++g_dropped_newest;
        // Caller's `ev` destructs as fn returns; release any image
        // refs it carries.
        std::string ds = ev.leader_source, dg = ev.group;   // the dropped (new) event
        for (auto& [src, h] : ev.images) xi::ImagePool::instance().release(h);
        lk.unlock();
        if (g_srv_for_bp)   // one Result per trigger: the dropped frame is NA
            emit_run_result(*g_srv_for_bp, XI_SYS_DROPPED, "dropped: queue full (drop_newest)", -1, -1, ds, dg);
        return false;
    }
    if (overflow == "block") {
        // Wait until at least one slot frees up. Bounded by g_continuous
        // so cmd:stop wakes us.
        g_ev_cv.wait(lk, [depth] {
            return (int)g_ev_queue.size() < depth || !g_continuous.load();
        });
        if (!g_continuous.load()) {
            for (auto& [src, h] : ev.images) xi::ImagePool::instance().release(h);
            return false;
        }
        g_ev_queue.push_back(std::move(ev));
        g_ev_cv.notify_one();
        return true;
    }
    // Default: drop_oldest.
    auto& front = g_ev_queue.front();
    std::string ds = front.leader_source, dg = front.group;   // the dropped (oldest) event
    for (auto& [src, h] : front.images) xi::ImagePool::instance().release(h);
    g_ev_queue.pop_front();
    g_ev_queue.push_back(std::move(ev));
    g_ev_cv.notify_one();
    ++g_dropped_oldest;
    lk.unlock();
    if (g_srv_for_bp)   // one Result per trigger: the dropped frame is NA
        emit_run_result(*g_srv_for_bp, XI_SYS_DROPPED, "dropped: queue full (drop_oldest)", -1, -1, ds, dg);
    return true;
}

static void warn_oversubscribe_(int total_workers);   // defined below (group section)

// Spawn the dispatcher pool + timer thread for cmd:start / hot-reload
// resume. `n_threads` comes from project.dispatch_threads (default 1).
// The timer thread pushes a synthetic empty trigger event at the
// requested fps so scripts without a real trigger source still see
// periodic dispatch. All N workers pull from the same g_ev_queue.
//
// Caller must have already set g_continuous = true and installed a
// TriggerBus sink that pushes into g_ev_queue.
static void spawn_dispatch_pool_(xi::ws::Server* srv_ptr,
                                 int interval_ms,
                                 int n_threads) {
    if (n_threads < 1) n_threads = 1;
    warn_oversubscribe_(n_threads);
    g_worker_threads.clear();
    g_worker_threads.reserve((size_t)n_threads);
    // Result ordering: arrival-ordered emission only when explicitly asked AND
    // there's actual concurrency (N>1 — N==1 is already in order). Reset the
    // sequence cursors so each (re)start counts from 0. Covers both cmd:start
    // and the hot-reload re-arm, since both come through here.
    bool ordered = (g_plugin_mgr.project().result_order == "arrival") && n_threads > 1;
    g_result_ordered.store(ordered);
    g_dispatch_seq.store(0);
    { std::lock_guard<std::mutex> lk(g_global_gate.mu); g_global_gate.next = 0; }
    std::fprintf(stderr,
        "[xinsp2] continuous mode: %dms timer + %d dispatcher thread(s) + trigger bus%s\n",
        interval_ms, n_threads, ordered ? " (arrival-ordered results)" : "");

    // N worker threads — each pops from g_ev_queue and dispatches.
    // run_one_inspection allocates its own run_id from g_run_id. Wire ordering
    // of vars / preview frames is by completion time by default; in
    // result_order:"arrival" mode each pop also claims a gapless emit sequence
    // (eseq, under g_ev_mu so it follows arrival order) that the EmitTurn gate
    // replays in order. The watchdog is per-worker now (each inspect arms its
    // own slot), so N>1 is covered too.
    auto worker_body = [srv_ptr] {
        reserve_fault_stack();   // BUG 2: dump survives a script stack overflow
        _set_se_translator(seh_translator);
        while (g_continuous.load()) {
            xi::TriggerEvent ev;
            bool have_ev = false;
            int64_t eseq = -1;
            int64_t rid  = 0;
            {
                std::unique_lock<std::mutex> lk(g_ev_mu);
                g_ev_cv.wait(lk, [] {
                    return !g_ev_queue.empty() || !g_continuous.load();
                });
                if (!g_continuous.load()) break;
                if (!g_ev_queue.empty()) {
                    ev = std::move(g_ev_queue.front());
                    g_ev_queue.pop_front();
                    have_ev = true;
                    // Assign run_id AND the emit sequence under the queue lock so
                    // both follow arrival/FIFO order exactly (run_id used to be
                    // assigned at inspect-start, which raced under N>1). eseq is
                    // only claimed in ordered mode.
                    rid = ++g_run_id;
                    if (g_result_ordered.load()) eseq = g_dispatch_seq.fetch_add(1);
                }
            }
            // A-P1-1: notify any producer that's waiting on
            // overflow:"block" (queue.size() == depth). Without this
            // the producer can stall forever — workers only ran
            // notify_one on push, not on pop. Wake one slot per pop.
            g_ev_cv.notify_one();
            if (!have_ev) continue;
            // Stamp the dequeue moment so the script can decompose
            // end-to-end latency into queue-wait vs inspect-time. Same
            // clock as ev.timestamp_us (xi::now_us() == system_clock us).
            // Done outside g_ev_mu — the field is owned by `ev` now,
            // and no other thread has a reference until we publish via
            // g_current_trigger below.
            ev.dequeued_at_us = xi::now_us();
            int frame_seq = (int)rid;   // arrival-order frame hint (== run_id)
            if (!ev.images.empty() || ev.id.hi || ev.id.lo) {
                g_current_trigger = &ev;
                run_one_inspection(*srv_ptr, frame_seq, rid, "", eseq);
                g_current_trigger = nullptr;
                for (auto& [src, h] : ev.images) xi::ImagePool::instance().release(h);
            } else {
                // Synthetic timer tick from g_timer_thread — no trigger.
                run_one_inspection(*srv_ptr, frame_seq, rid, "", eseq);
            }
        }
    };
    for (int i = 0; i < n_threads; ++i) {
        g_worker_threads.emplace_back(worker_body);
    }

    // Timer thread: every interval_ms push a synthetic empty event so scripts
    // without trigger sources still tick. interval_ms <= 0 => trigger-only mode:
    // no timer at all, the project's sources are the sole dispatch driver.
    if (interval_ms > 0) {
        g_timer_thread = std::thread([interval_ms] {
            while (g_continuous.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
                if (!g_continuous.load()) break;
                (void)enqueue_event_(xi::TriggerEvent{});
            }
        });
    }
}

// ---- Dispatch groups: per-group worker lanes (gated on parallelism.groups) ----
// Each group owns its own queue + max_parallel worker threads at its OS
// thread_priority, draining only its own queue. Only active when the project
// declares groups; otherwise the legacy single pool above is used UNCHANGED.
// Result ordering is per-lane completion order in v1 (per-group arrival + the
// `group` wire tag are follow-ups). See docs/design/dispatch-groups.md.
struct GroupLane {
    xi::ProjectInfo::DispatchGroup cfg;
    std::deque<xi::TriggerEvent>   q;
    std::mutex                     mu;
    std::condition_variable        cv;
    std::vector<std::thread>       workers;
    std::atomic<uint64_t>          running{0};
    std::atomic<uint64_t>          dropped{0};
    std::atomic<uint64_t>          high_watermark{0};
    // Per-group result ordering (result_order: "arrival"). `ordered` is set at
    // spawn (arrival && max_parallel>1). Each dequeue claims a gapless seq from
    // seq_next under mu (so it follows arrival order); `gate` replays emission in
    // that order — independent of other groups' gates.
    bool                           ordered{false};
    std::atomic<int64_t>           seq_next{0};
    EmitGate                       gate;
};
// Lanes are shared_ptr + guarded by g_lanes_mu so a producer (an emit thread /
// the timer) that grabbed a lane can't have it destroyed under it by a concurrent
// stop_group_pool_ — the shared_ptr keeps the GroupLane (its mutex/cv) alive until
// the producer is done. Fixes the lane-lifetime UAF found in v1 hardening.
static std::vector<std::shared_ptr<GroupLane>> g_lanes;
static std::mutex                              g_lanes_mu;
static bool grouping_enabled_() { return !g_plugin_mgr.project().groups.empty(); }

static void set_os_thread_priority_(const std::string& p) {
#ifdef _WIN32
    int pr = THREAD_PRIORITY_NORMAL;
    if      (p == "high") pr = THREAD_PRIORITY_ABOVE_NORMAL;
    else if (p == "low")  pr = THREAD_PRIORITY_BELOW_NORMAL;
    SetThreadPriority(GetCurrentThread(), pr);
#else
    (void)p;   // TODO(linux): pthread_setschedprio / nice
#endif
}

// Pin the current thread to a set of cores (a MASK — the thread may run on ANY of
// them, not just one). Empty `cores` = leave unbound. Bogus core ids are dropped
// (intersected with the process's allowed mask) so a bad config can't wipe the
// affinity to nothing.
static void set_os_thread_affinity_(const std::vector<int>& cores) {
    if (cores.empty()) return;
#ifdef _WIN32
    DWORD_PTR want = 0;
    for (int c : cores) if (c >= 0 && c < 64) want |= (DWORD_PTR(1) << c);  // 64-bit mask (≤64 cores; >64 = processor groups, TODO)
    if (!want) return;
    DWORD_PTR procMask = 0, sysMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask)) want &= procMask;
    if (want) SetThreadAffinityMask(GetCurrentThread(), want);
#else
    (void)cores;   // TODO(linux): cpu_set_t + pthread_setaffinity_np / sched_setaffinity
#endif
}

// Warn (once per start) if the total dispatch worker count exceeds the core count.
// Oversubscription causes context-switch thrash that usually slows inspects — a
// dedicated inspection PC should keep Σ max_parallel ≤ cores (minus a couple for
// the FE supervisor / comms gateway / OS).
static void warn_oversubscribe_(int total_workers) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw > 0 && total_workers > (int)hw)
        std::fprintf(stderr,
            "[xinsp2] WARNING: %d dispatch worker threads on %u cores (oversubscribed) — "
            "context-switch thrash may slow inspects; lower max_parallel or add cores\n",
            total_workers, hw);
}

// Resolve a group name to its lane (holding g_lanes_mu). Unknown/typo'd group →
// the default_group lane, then the first lane — never silently the front (#5).
// Returns a shared_ptr so the caller keeps the lane alive past a concurrent stop.
static std::shared_ptr<GroupLane> lane_for_(const std::string& group) {
    std::lock_guard<std::mutex> lk(g_lanes_mu);
    if (g_lanes.empty()) return nullptr;
    for (auto& l : g_lanes) if (l->cfg.name == group) return l;
    const std::string& dg = g_plugin_mgr.project().default_group;
    if (!dg.empty()) for (auto& l : g_lanes) if (l->cfg.name == dg) return l;
    return g_lanes.front();
}

// Per-lane enqueue with that lane's queue_depth/overflow (mirrors enqueue_event_).
static bool enqueue_to_lane_(xi::TriggerEvent ev) {
    auto rel = [&] { for (auto& [s, h] : ev.images) xi::ImagePool::instance().release(h); };
    if (!g_continuous.load()) { rel(); return false; }
    std::shared_ptr<GroupLane> lane = lane_for_(ev.group);
    if (!lane) { rel(); return false; }
    int depth = lane->cfg.queue_depth < 1 ? 1 : lane->cfg.queue_depth;
    const std::string& ov = lane->cfg.overflow;
    std::unique_lock<std::mutex> lk(lane->mu);
    // Re-check after taking the lane lock: a concurrent stop may have flipped
    // g_continuous + drained; don't push a now-orphaned event (would leak).
    if (!g_continuous.load()) { rel(); return false; }
    if ((int)lane->q.size() < depth) {
        lane->q.push_back(std::move(ev));
        uint64_t ns = lane->q.size(), prev = lane->high_watermark.load(std::memory_order_relaxed);
        while (ns > prev && !lane->high_watermark.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {}
        lane->cv.notify_one(); return true;
    }
    if (ov == "drop_newest") {
        ++lane->dropped;
        std::string ds = ev.leader_source, dg = ev.group;   // the dropped (new) event
        for (auto& [s, h] : ev.images) xi::ImagePool::instance().release(h);
        lk.unlock();
        if (g_srv_for_bp)
            emit_run_result(*g_srv_for_bp, XI_SYS_DROPPED, "dropped: queue full (drop_newest)", -1, -1, ds, dg);
        return false;
    }
    if (ov == "block") {
        lane->cv.wait(lk, [&] { return (int)lane->q.size() < depth || !g_continuous.load(); });
        if (!g_continuous.load()) { for (auto& [s, h] : ev.images) xi::ImagePool::instance().release(h); return false; }
        lane->q.push_back(std::move(ev)); lane->cv.notify_one(); return true;
    }
    auto& front = lane->q.front();   // drop_oldest
    std::string ds = front.leader_source, dg = front.group;   // the dropped (oldest) event
    for (auto& [s, h] : front.images) xi::ImagePool::instance().release(h);
    lane->q.pop_front(); lane->q.push_back(std::move(ev)); lane->cv.notify_one(); ++lane->dropped;
    lk.unlock();
    if (g_srv_for_bp)
        emit_run_result(*g_srv_for_bp, XI_SYS_DROPPED, "dropped: queue full (drop_oldest)", -1, -1, ds, dg);
    return true;
}

static void spawn_group_pool_(xi::ws::Server* srv_ptr, int interval_ms) {
    {
        std::lock_guard<std::mutex> lk(g_lanes_mu);
        g_lanes.clear();
        for (auto& gc : g_plugin_mgr.project().groups) {
            auto lane = std::make_shared<GroupLane>(); lane->cfg = gc; g_lanes.push_back(std::move(lane));
        }
    }
    std::fprintf(stderr, "[xinsp2] continuous mode (grouped): %zu group(s), %dms timer\n",
                 g_lanes.size(), interval_ms);
    { int total = 0; for (auto& lp : g_lanes) total += (lp->cfg.max_parallel < 1 ? 1 : lp->cfg.max_parallel);
      warn_oversubscribe_(total); }
    for (auto& lp : g_lanes) {
        std::shared_ptr<GroupLane> lane = lp;   // workers hold a ref → lane outlives them
        int n = lane->cfg.max_parallel < 1 ? 1 : lane->cfg.max_parallel;
        // Arrival-ordered emission only when asked AND there's real concurrency
        // (n==1 is already in order). Reset the per-lane cursors per (re)start.
        lane->ordered = (lane->cfg.result_order == "arrival") && n > 1;
        lane->seq_next.store(0);
        { std::lock_guard<std::mutex> lk(lane->gate.mu); lane->gate.next = 0; }
        for (int i = 0; i < n; ++i) {
            lane->workers.emplace_back([srv_ptr, lane, wi = i] {
                reserve_fault_stack();
                _set_se_translator(seh_translator);
                set_os_thread_priority_(lane->cfg.thread_priority);
                // CPU affinity (empty = unbound). One mask → all workers share it;
                // N masks → worker wi uses set[wi % N]. Each mask may be multi-core.
                const auto& aff = lane->cfg.cpu_affinity;
                if (!aff.empty())
                    set_os_thread_affinity_(aff.size() == 1 ? aff[0] : aff[(size_t)wi % aff.size()]);
                while (g_continuous.load()) {
                    xi::TriggerEvent ev; bool have = false; int64_t rid = 0; int64_t eseq = -1;
                    {
                        std::unique_lock<std::mutex> lk(lane->mu);
                        lane->cv.wait(lk, [lane] { return !lane->q.empty() || !g_continuous.load(); });
                        if (!g_continuous.load()) break;
                        if (!lane->q.empty()) {
                            ev = std::move(lane->q.front()); lane->q.pop_front(); have = true; rid = ++g_run_id;
                            // Claim the emit seq under the queue lock → follows dequeue
                            // (== FIFO arrival) order. Only dequeued events get a seq, so
                            // dropped frames leave no gap in the gate's sequence.
                            if (lane->ordered) eseq = lane->seq_next.fetch_add(1);
                        }
                    }
                    lane->cv.notify_one();   // wake a producer parked on overflow:block
                    if (!have) continue;
                    ev.dequeued_at_us = xi::now_us();
                    lane->running.fetch_add(1);
                    int frame_seq = (int)rid;
                    if (!ev.images.empty() || ev.id.hi || ev.id.lo) {
                        g_current_trigger = &ev;
                        run_one_inspection(*srv_ptr, frame_seq, rid, "", eseq, &lane->gate);
                        g_current_trigger = nullptr;
                        for (auto& [s, h] : ev.images) xi::ImagePool::instance().release(h);
                    } else {
                        run_one_inspection(*srv_ptr, frame_seq, rid, "", eseq, &lane->gate);
                    }
                    lane->running.fetch_sub(1);
                }
            });
        }
    }
    // Timer ticks feed the default group's lane. interval_ms <= 0 => trigger-only:
    // no timer, so the default group isn't loaded with synthetic ticks (the sources
    // are the sole driver).
    if (interval_ms > 0) {
        g_timer_thread = std::thread([interval_ms] {
            const std::string dg = g_plugin_mgr.project().default_group;
            while (g_continuous.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
                if (!g_continuous.load()) break;
                xi::TriggerEvent ev; ev.group = dg;
                (void)enqueue_to_lane_(std::move(ev));
            }
        });
    }
}

static void stop_group_pool_() {
    // Snapshot the lanes (under the lock) so producers can keep routing into the
    // shared_ptrs while we tear down; new enqueues already bail on !g_continuous.
    std::vector<std::shared_ptr<GroupLane>> lanes;
    { std::lock_guard<std::mutex> lk(g_lanes_mu); lanes = g_lanes; }
    for (auto& lp : lanes) { std::lock_guard<std::mutex> lk(lp->mu); lp->cv.notify_all(); }
    for (auto& lp : lanes) for (auto& t : lp->workers) if (t.joinable()) t.join();
    // Workers are gone + g_continuous is false → drain leftover queued events and
    // release their image handles before the lanes are dropped (mirrors the legacy
    // g_ev_queue drain; preserves release-before-FreeLibrary). (#3 leak fix.)
    for (auto& lp : lanes) {
        std::lock_guard<std::mutex> lk(lp->mu);
        for (auto& ev : lp->q) for (auto& [s, h] : ev.images) xi::ImagePool::instance().release(h);
        lp->q.clear();
    }
    { std::lock_guard<std::mutex> lk(g_lanes_mu); g_lanes.clear(); }
}

// Stop the pool + timer. Safe to call if nothing was spawned.
static void stop_dispatch_pool_() {
    g_continuous = false;
    g_ev_cv.notify_all();
    g_global_gate.cv.notify_all();   // wake any legacy-pool worker parked in an EmitTurn
    // Wake grouped workers + any producer (incl. the timer) parked in a lane's
    // overflow:block BEFORE joining the timer, or the join deadlocks. Also wake
    // anyone parked in a per-lane EmitTurn (ordered mode).
    {
        std::vector<std::shared_ptr<GroupLane>> lanes;
        { std::lock_guard<std::mutex> lk(g_lanes_mu); lanes = g_lanes; }
        for (auto& lp : lanes) {
            { std::lock_guard<std::mutex> lk(lp->mu); lp->cv.notify_all(); }
            { std::lock_guard<std::mutex> lk(lp->gate.mu); lp->gate.cv.notify_all(); }
        }
    }
    if (g_timer_thread.joinable()) g_timer_thread.join();
    for (auto& t : g_worker_threads) {
        if (t.joinable()) t.join();
    }
    g_worker_threads.clear();
    stop_group_pool_();
}

// Trigger-driven dispatch WITHOUT continuous mode: a source emitting a trigger
// (e.g. a webui "issue"/"replay" click) runs exactly ONE inspect on it. The emit
// usually arrives on the WS thread (inside a plugin's exchange), so we run the
// inspect on a detached thread, not inline. Serialized by g_run_mu; the
// thread_local g_current_trigger makes this thread's inspect see this event.
static void dispatch_one_shot_(xi::ws::Server* srv, xi::TriggerEvent ev) {
    auto evp = std::make_shared<xi::TriggerEvent>(std::move(ev));
    std::thread([srv, evp]() {
        reserve_fault_stack();
        _set_se_translator(seh_translator);
        std::lock_guard<std::mutex> lk(g_run_mu);
        g_current_trigger = evp.get();
        run_one_inspection(*srv, /*frame_hint=*/0, /*run_id=*/0, "", /*emit_seq=*/-1);
        g_current_trigger = nullptr;
        for (auto& [src, h] : evp->images) xi::ImagePool::instance().release(h);
    }).detach();
}

// Install the bus sink so triggers always dispatch: in continuous mode they feed
// the worker-pool queue; otherwise each trigger runs a single-shot inspect. This
// is installed on every compile_and_load so "issue"/"replay" works WITHOUT needing
// cmd:start — the trigger-driven model (continuous is just an optional free-running
// timer on top).
static void install_trigger_sink_(xi::ws::Server* srv) {
    xi::TriggerBus::instance().set_sink([srv](xi::TriggerEvent ev) {
        if (g_continuous.load()) {
            if (grouping_enabled_()) {
                // Route by the emitting source instance's "group" (default_group
                // if the source is untagged or unknown / synthetic timer tick).
                std::string g;
                if (!ev.leader_source.empty()) {
                    auto& insts = g_plugin_mgr.project().instances;
                    auto it = insts.find(ev.leader_source);
                    if (it != insts.end()) g = it->second.group;
                }
                if (g.empty()) g = g_plugin_mgr.project().default_group;
                ev.group = g;
                (void)enqueue_to_lane_(std::move(ev));
            } else {
                (void)enqueue_event_(std::move(ev));
            }
        } else {
            dispatch_one_shot_(srv, std::move(ev));
        }
    });
}

// Quiesce the dispatcher + timer + breakpoint park before any handler
// that's about to touch the script DLL or project plugin DLLs (audit
// P0-AB-1..5). Returns the prior continuous state so the caller can
// re-arm if it wants. Mirrors the inline block compile_and_load has
// used since FL r5.
//
// IMPORTANT: this only quiesces the bus-driven dispatcher pool. Any
// detached cmd:run threads are not joined here — those snapshot the
// script under g_script_mu and the destructive caller still holds
// g_script_mu (or equivalent) while the inspect runs to completion.
// For project plugin DLLs (touched by close/open/recompile/export),
// no equivalent detached path exists; the dispatch pool is the only
// in-flight surface.
struct DispatchPoolGuard {
    bool was_continuous = false;
    int  prior_fps = 10;
    bool quiesced = false;
};

static DispatchPoolGuard quiesce_dispatch_for_lifecycle_op_(const char* op_name) {
    DispatchPoolGuard g;
    if (g_continuous.load()) {
        g.was_continuous = true;
        g.prior_fps = g_continuous_fps.load();
        // Release any breakpoint-parked thread so it can finish.
        { std::lock_guard<std::mutex> lk(g_bp_mu); g_bp_paused = false; }
        g_bp_cv.notify_all();
        stop_dispatch_pool_();
        g.quiesced = true;
        std::fprintf(stderr,
            "[xinsp2] stopped continuous mode for %s (will resume if op succeeds)\n",
            op_name);
    }
    // Drain any events still in g_ev_queue; they reference plugin
    // images whose handles must be released before the plugin DLL is
    // unloaded, otherwise the eventual ImagePool sweep races with
    // FreeLibrary.
    {
        std::lock_guard<std::mutex> lk(g_ev_mu);
        for (auto& ev : g_ev_queue) {
            for (auto& [src, h] : ev.images) {
                xi::ImagePool::instance().release(h);
            }
        }
        g_ev_queue.clear();
    }
    return g;
}

static void handle_command(xi::ws::Server& srv, std::string_view text) {
    auto parsed = xp::parse_cmd(text);
    if (!parsed) {
        xp::LogMsg lm;
        lm.level = "error";
        lm.msg   = std::string("malformed cmd: ") + std::string(text.substr(0, 128));
        srv.send_text(lm.to_json());
        return;
    }

    const auto& name = parsed->name;
    const int64_t id = parsed->id;

    if (name == "ping") {
        char buf[128];
        std::snprintf(buf, sizeof(buf), R"({"pong":true,"ts":%.3f})", now_seconds());
        send_rsp_ok(srv, id, buf);
    } else if (name == "version") {
        std::string vd = std::string(R"({"version":")") + XINSP2_VERSION
                       + R"(","commit":")" + XINSP2_COMMIT
                       + R"(","abi":1})";
        send_rsp_ok(srv, id, vd);
    } else if (name == "subscribe") {
        // args: { names: [...] } OR { all: true }
        bool want_all = parsed->args_json.find("\"all\":true") != std::string::npos;
        std::unordered_set<std::string> names;
        if (!want_all) {
            // Parse the names array. cJSON is simpler than hand-rolled here.
            cJSON* root = cJSON_Parse(parsed->args_json.c_str());
            if (root) {
                cJSON* arr = cJSON_GetObjectItem(root, "names");
                if (cJSON_IsArray(arr)) {
                    int n = cJSON_GetArraySize(arr);
                    for (int i = 0; i < n; ++i) {
                        cJSON* it = cJSON_GetArrayItem(arr, i);
                        if (cJSON_IsString(it) && it->valuestring) names.insert(it->valuestring);
                    }
                }
                cJSON_Delete(root);
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_sub_mu);
            g_sub_all = want_all;
            g_sub_names = std::move(names);
        }
        std::string out = "{\"all\":";
        out += want_all ? "true" : "false";
        out += ",\"count\":";
        {
            std::lock_guard<std::mutex> lk(g_sub_mu);
            out += std::to_string(g_sub_names.size());
        }
        out += "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "crash_reports") {
        // List crash JSON reports left by previous fatal crashes.
        // Returns the file contents inline (each is small, KB-sized).
        namespace fs = std::filesystem;
        auto dir = fs::temp_directory_path() / "xinsp2" / "crashdumps";
        std::string out = "{\"reports\":[";
        bool first = true;
        std::error_code ec;
        if (fs::exists(dir, ec)) {
            std::vector<fs::directory_entry> entries;
            for (auto& e : fs::directory_iterator(dir, ec)) {
                if (e.path().extension() == ".json") entries.push_back(e);
            }
            // Sort newest-first by mtime
            std::sort(entries.begin(), entries.end(),
                [](auto& a, auto& b) {
                    std::error_code ec2;
                    return fs::last_write_time(a.path(), ec2) > fs::last_write_time(b.path(), ec2);
                });
            for (auto& e : entries) {
                std::ifstream f(e.path(), std::ios::binary);
                std::stringstream ss; ss << f.rdbuf();
                std::string body = ss.str();
                while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
                if (body.empty() || body[0] != '{') continue;
                if (!first) out += ",";
                first = false;
                out += "{\"file\":";
                xp::json_escape_into(out, e.path().filename().string());
                out += ",\"report\":";
                out += body;
                out += "}";
            }
        }
        out += "]}";
        send_rsp_ok(srv, id, out);
    } else if (name == "clear_crash_reports") {
        namespace fs = std::filesystem;
        auto dir = fs::temp_directory_path() / "xinsp2" / "crashdumps";
        int n = 0;
        std::error_code ec;
        if (fs::exists(dir, ec)) {
            for (auto& e : fs::directory_iterator(dir, ec)) {
                fs::remove(e.path(), ec);
                ++n;
            }
        }
        send_rsp_ok(srv, id, "{\"removed\":" + std::to_string(n) + "}");
    } else if (name == "compare_variants") {
        // S7: apply variant A → run → snapshot → apply B → run → snapshot.
        // args: { a: {params:[...], instances:[...]}, b: {...} }
        //   params:    [{name, value}]        (value is JSON scalar)
        //   instances: [{name, def}]          (def is JSON object)
        // Reply: { a: {vars: <snap>}, b: {vars: <snap>} }
        //
        // The client drives comparison — we just guarantee atomic back-to-
        // back runs with consistent variant state. A successful run
        // leaves the script in variant-B state; the caller restores
        // their own default with a follow-up set_param / load_project.
        auto apply_variant = [&](cJSON* root) -> bool {
            if (!root || !g_script.ok()) return false;
            cJSON* params = cJSON_GetObjectItem(root, "params");
            if (cJSON_IsArray(params) && g_script.set_param) {
                int n = cJSON_GetArraySize(params);
                for (int i = 0; i < n; ++i) {
                    cJSON* p = cJSON_GetArrayItem(params, i);
                    cJSON* nm = cJSON_GetObjectItem(p, "name");
                    cJSON* vv = cJSON_GetObjectItem(p, "value");
                    if (!cJSON_IsString(nm) || !vv) continue;
                    char* val = cJSON_PrintUnformatted(vv);
                    if (val) { g_script.set_param(nm->valuestring, val); cJSON_free(val); }
                }
            }
            cJSON* insts = cJSON_GetObjectItem(root, "instances");
            if (cJSON_IsArray(insts)) {
                int n = cJSON_GetArraySize(insts);
                for (int i = 0; i < n; ++i) {
                    cJSON* it = cJSON_GetArrayItem(insts, i);
                    cJSON* nm = cJSON_GetObjectItem(it, "name");
                    cJSON* df = cJSON_GetObjectItem(it, "def");
                    if (!cJSON_IsString(nm) || !df) continue;
                    char* def_str = cJSON_PrintUnformatted(df);
                    if (!def_str) continue;
                    auto inst = xi::InstanceRegistry::instance().find(nm->valuestring);
                    if (inst) inst->set_def(def_str);
                    else if (g_script.set_instance_def)
                        g_script.set_instance_def(nm->valuestring, def_str);
                    cJSON_free(def_str);
                }
            }
            return true;
        };
        auto run_and_snapshot = [&]() -> std::string {
            if (!g_script.ok() || !g_script.inspect) return "[]";
            if (g_script.reset) g_script.reset();
            try { g_script.inspect(0); }
            catch (...) { /* best-effort: keep going */ }
            if (!g_script.snapshot) return "[]";
            std::vector<char> buf(256 * 1024);
            int n = g_script.snapshot(buf.data(), (int)buf.size());
            if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024);
                         n = g_script.snapshot(buf.data(), (int)buf.size()); }
            return n > 0 ? std::string(buf.data(), (size_t)n) : std::string("[]");
        };
        cJSON* root = cJSON_Parse(parsed->args_json.c_str());
        if (!root) { send_rsp_err(srv, id, "invalid args JSON"); return; }
        cJSON* a = cJSON_GetObjectItem(root, "a");
        cJSON* b = cJSON_GetObjectItem(root, "b");
        if (!a || !b) { cJSON_Delete(root); send_rsp_err(srv, id, "need args.a and args.b"); return; }
        std::string snap_a, snap_b;
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (!g_script.ok()) { cJSON_Delete(root); send_rsp_err(srv, id, "no script loaded"); return; }
            apply_variant(a);
            snap_a = run_and_snapshot();
            apply_variant(b);
            snap_b = run_and_snapshot();
        }
        cJSON_Delete(root);
        std::string out = "{\"a\":{\"vars\":";
        out += snap_a;
        out += "},\"b\":{\"vars\":";
        out += snap_b;
        out += "}}";
        send_rsp_ok(srv, id, out);
    } else if (name == "set_watchdog_ms") {
        // P2.4. Set the wall-clock budget (ms) for a single inspect()
        // call. 0 disables. Tripping the watchdog does not auto-reset —
        // the next inspect re-arms with the new budget.
        auto ms_opt = xp::get_number_field(parsed->args_json, "ms");
        int ms = ms_opt ? (int)*ms_opt : 0;
        if (ms < 0) ms = 0;
        if (ms > 600000) ms = 600000;     // 10-minute hard cap
        g_watchdog_ms = ms;
        std::string out = "{\"ms\":" + std::to_string(ms);
        out += ",\"trips\":" + std::to_string(g_watchdog_trips.load()) + "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "watchdog_status") {
        std::string out = "{\"ms\":" + std::to_string(g_watchdog_ms.load());
        out += ",\"trips\":" + std::to_string(g_watchdog_trips.load());
        out += ",\"armed\":";
        // armed == at least one inspect slot is currently in flight.
        bool armed = false;
        for (int i = 0; i < WD_SLOTS; ++i) if (g_wd_deadlines[i].load() != 0) { armed = true; break; }
        out += (armed ? "true" : "false");
        out += "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "resume") {
        // S3: unblock a script waiting in xi::breakpoint(). Idempotent —
        // calling when not paused is a no-op with an informative reply.
        std::string out;
        {
            std::lock_guard<std::mutex> lk(g_bp_mu);
            if (g_bp_paused) {
                g_bp_paused = false;
                out = "{\"resumed\":true,\"label\":";
                xp::json_escape_into(out, g_bp_last_label);
                out += "}";
            } else {
                out = "{\"resumed\":false}";
            }
        }
        g_bp_cv.notify_all();
        send_rsp_ok(srv, id, out);
    } else if (name == "history") {
        // S4: return the most recent N vars snapshots (default: all kept).
        // args: { count?: N, since_run_id?: id }
        auto cnt_opt = xp::get_number_field(parsed->args_json, "count");
        auto since_opt = xp::get_number_field(parsed->args_json, "since_run_id");
        size_t want = cnt_opt ? (size_t)std::max(0, (int)*cnt_opt) : (size_t)-1;
        int64_t since = since_opt ? (int64_t)*since_opt : 0;
        std::string out = "{\"depth\":";
        {
            std::lock_guard<std::mutex> lk(g_hist_mu);
            out += std::to_string(g_hist_max);
            out += ",\"size\":";
            out += std::to_string(g_history.size());
            out += ",\"runs\":[";
            // Walk newest-to-oldest until we've collected `want` or exhausted.
            size_t emitted = 0;
            bool first = true;
            for (auto it = g_history.rbegin(); it != g_history.rend(); ++it) {
                if (emitted >= want) break;
                if (it->run_id <= since) break;
                if (!first) out += ",";
                first = false;
                out += "{\"run_id\":" + std::to_string(it->run_id);
                out += ",\"ts_ms\":" + std::to_string(it->ts_ms);
                out += ",\"vars\":" + it->vars_json;
                out += "}";
                ++emitted;
            }
            out += "]}";
        }
        send_rsp_ok(srv, id, out);
    } else if (name == "clear_history") {
        size_t cleared = 0;
        {
            std::lock_guard<std::mutex> lk(g_hist_mu);
            cleared = g_history.size();
            g_history.clear();
        }
        send_rsp_ok(srv, id, "{\"cleared\":" + std::to_string(cleared) + "}");
    } else if (name == "set_history_depth") {
        auto d = xp::get_number_field(parsed->args_json, "depth");
        if (!d) { send_rsp_err(srv, id, "missing depth"); return; }
        int n = (int)*d;
        if (n < 0) n = 0;
        if (n > 10000) n = 10000;
        {
            std::lock_guard<std::mutex> lk(g_hist_mu);
            g_hist_max = (size_t)n;
            while (g_history.size() > g_hist_max) g_history.pop_front();
        }
        send_rsp_ok(srv, id, "{\"depth\":" + std::to_string(n) + "}");
    } else if (name == "unsubscribe") {
        {
            std::lock_guard<std::mutex> lk(g_sub_mu);
            g_sub_all = false;
            g_sub_names.clear();
        }
        send_rsp_ok(srv, id, R"({"all":false,"count":0})");
    } else if (name == "shutdown") {
        // Stop continuous mode first to avoid use-after-free on srv
        if (g_continuous.load()) {
            stop_dispatch_pool_();
        }
        send_rsp_ok(srv, id);
        g_should_exit = true;
    } else if (name == "compile_and_load") {
        auto src = xp::get_string_field(parsed->args_json, "path");
        if (!src) {
            send_rsp_err(srv, id, "compile_and_load: missing path");
            return;
        }

        // Stop continuous mode before reloading — the worker thread holds
        // function pointers into the DLL we're about to unload. Also
        // release any breakpoint that's currently parked, so the worker
        // can actually finish. Remember whether the run was active so
        // we can re-arm it after the reload — without this, scripts
        // that get hot-reloaded mid-run would silently halt and the
        // caller would have to know to re-issue cmd:start.
        bool was_continuous = false;
        int  prior_continuous_fps = 10;
        if (g_continuous.load()) {
            was_continuous = true;
            prior_continuous_fps = g_continuous_fps.load();
            { std::lock_guard<std::mutex> lk(g_bp_mu); g_bp_paused = false; }
            g_bp_cv.notify_all();
            stop_dispatch_pool_();
            std::fprintf(stderr, "[xinsp2] stopped continuous mode for reload (will resume)\n");
        }

        xi::script::CompileRequest req;
        req.source_path     = *src;
        req.output_dir      = (std::filesystem::path(g_work_dir) / "script_build").string();
        req.include_dir     = g_include_dir;
        req.opencv_dir      = g_opencv_dir;
        req.turbojpeg_root  = g_turbojpeg_root;
        req.ipp_root        = g_ipp_root;
        req.vcvars_path     = g_tc_vcvars;   // empty = compiler auto-finds vcvars64.bat
        // Project-declared external deps (project.json include_dirs / link_libs).
        read_script_deps_(g_project_folder, req.include_dirs, req.link_libs);
        // Fast dev compile (/Od) by default — the interactive edit→run loop wants
        // fast COMPILE, not fast runtime. A client benchmarking / the autostart
        // boot path passes "optimize":true to get /O2. (Both spacings, like has_ui.)
        bool want_optimize = parsed->args_json.find("\"optimize\":true") != std::string::npos ||
                             parsed->args_json.find("\"optimize\": true") != std::string::npos;
        req.fast = !want_optimize;

        // P2-6: emit a `compile_started` event before kicking off cl.exe.
        // compile_and_load is a request/response that can take 4+ s on a
        // fresh checkout; without this event drivers see a silent WS for
        // multiple seconds and can't show "compiling..." UI. data carries
        // the source path so concurrent compiles can be disambiguated.
        {
            xp::Event ev;
            ev.name = "compile_started";
            std::string data = "{\"path\":";
            xp::json_escape_into(data, *src);
            data += "}";
            ev.data_json = data;
            srv.send_text(ev.to_json());
        }

        auto res = xi::script::compile(req);

        // Pair the started event with a finished event so drivers can
        // bracket the operation. Carries `ok` and a short summary so a
        // listener that missed the rsp can still tell success from
        // failure.
        {
            xp::Event ev;
            ev.name = "compile_finished";
            std::string data = "{\"path\":";
            xp::json_escape_into(data, *src);
            data += ",\"ok\":";
            data += (res.ok ? "true" : "false");
            data += "}";
            ev.data_json = data;
            srv.send_text(ev.to_json());
        }

        // Serialize diagnostics for both error & success paths so the
        // extension can drive Problems panel / squiggles either way.
        auto build_diag_json = [&]() -> std::string {
            std::string s = "[";
            for (size_t i = 0; i < res.diagnostics.size(); ++i) {
                auto& d = res.diagnostics[i];
                if (i) s += ",";
                s += "{\"file\":";  xp::json_escape_into(s, d.file);
                s += ",\"line\":" + std::to_string(d.line);
                s += ",\"col\":"  + std::to_string(d.col);
                s += ",\"severity\":"; xp::json_escape_into(s, d.severity);
                s += ",\"code\":";    xp::json_escape_into(s, d.code);
                s += ",\"message\":"; xp::json_escape_into(s, d.message);
                s += "}";
            }
            s += "]";
            return s;
        };

        if (!res.ok) {
            std::string data = "{\"diagnostics\":" + build_diag_json() + "}";
            xp::Rsp r;
            r.id = id;
            r.ok = false;
            r.error = "compile failed";
            r.data_json = data;
            srv.send_text(r.to_json());
            xp::LogMsg lm;
            lm.level = "error";
            lm.msg = res.build_log;
            srv.send_text(lm.to_json());
            return;
        }

        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            // Save persistent state before unloading old DLL.
            // Stamp the OLD DLL's schema version alongside so restore
            // into the new DLL can detect a shape mismatch.
            if (g_script.ok() && g_script.get_state) {
                std::vector<char> buf(256 * 1024);
                int n = g_script.get_state(buf.data(), (int)buf.size());
                if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024);
                             n = g_script.get_state(buf.data(), (int)buf.size()); }
                if (n > 0) g_persistent_state_json.assign(buf.data(), (size_t)n);
                g_persistent_state_schema = g_script.state_schema_version
                                          ? g_script.state_schema_version()
                                          : 0;
            }
            xi::script::unload_script(g_script);
            // Reset cross-script transient state — the new DLL may
            // expose a different VAR set, so old subscription names and
            // historical run snapshots no longer match cleanly.
            {
                std::lock_guard<std::mutex> sl(g_sub_mu);
                g_sub_all = true;
                g_sub_names.clear();
            }
            {
                std::lock_guard<std::mutex> hl(g_hist_mu);
                g_history.clear();
            }
            std::string err;
            if (!xi::script::load_script(res.dll_path, g_script, err)) {
                send_rsp_err(srv, id, err);
                return;
            }
            crash_set(crash_ctx().last_script, sizeof(crash_ctx().last_script),
                      res.dll_path.c_str());
            crash_set(crash_ctx().last_cmd, sizeof(crash_ctx().last_cmd),
                      "compile_and_load");
            // Wire xi::use() callbacks so the script can call back into backend.
            // host_api lets the script allocate/read images in the BACKEND pool —
            // plugins only see that pool via their own host_api, so script-side
            // ImagePool handles would be invisible to them.
            if (g_script.set_use_callbacks) {
                static xi_host_api use_host = []{ auto a = xi::ImagePool::make_host_api(); xi::install_trigger_hook(a); return a; }();
                g_script.set_use_callbacks(
                    (void*)use_process_cb,
                    (void*)use_exchange_cb,
                    (void*)use_grab_cb,
                    (void*)&use_host);
            }
            // Phase 3: trigger access callbacks. Older scripts that don't
            // import xi_script_set_trigger_callbacks just stay null and
            // xi::current_trigger().is_active() returns false.
            if (g_script.set_trigger_callbacks) {
                g_script.set_trigger_callbacks(
                    (void*)trigger_info_cb,
                    (void*)trigger_image_cb,
                    (void*)trigger_sources_cb);
            }
            // Optional newer symbol — scripts compiled before P2-2
            // simply don't export it and t.primary_source() falls back
            // to sources().front().
            if (g_script.set_trigger_leader_callback) {
                g_script.set_trigger_leader_callback((void*)trigger_leader_cb);
            }
            // S3: breakpoint callback. Scripts without xi_breakpoint.hpp
            // leave this null and xi::breakpoint() is a no-op.
            if (g_script.set_breakpoint_callback) {
                g_script.set_breakpoint_callback((void*)breakpoint_cb);
            }
            // Status callback. Scripts without xi_status.hpp leave this null
            // and xi::status() is a no-op.
            if (g_script.set_status_callback) {
                g_script.set_status_callback((void*)status_cb);
            }
            // Result callback. Scripts without xi_result.hpp leave this null
            // and xi::result() is a no-op (run_result then defaults to NA).
            if (g_script.set_result_callback) {
                g_script.set_result_callback((void*)result_cb);
            }
            // Comms-gateway callbacks for xi::comms::* (no-op without xi_comms.hpp).
            if (g_script.set_comms_callbacks) {
                g_script.set_comms_callbacks((void*)comms_send_cb, (void*)comms_poll_cb,
                                             (void*)comms_up_cb, (void*)comms_deadman_cb);
            }
            // Replay any param values the user set on the previous
            // DLL. The new DLL's xi::Param<T> file-scope ctors run on
            // load and seed registry slots with default values; we
            // overwrite each one whose name we've seen via cmd:set_param
            // since the project opened. set_param returns -1 for
            // params the new DLL doesn't declare (renamed / deleted) —
            // those entries stay in the cache but quietly no-op until
            // the user hits set_param again, which is the right
            // failure mode (no false positives, no spurious errors).
            if (g_script.set_param) {
                for (auto& [pname, pval] : g_param_cache) {
                    g_script.set_param(pname.c_str(), pval.c_str());
                }
                if (!g_param_cache.empty()) {
                    std::fprintf(stderr,
                        "[xinsp2] replayed %zu param value(s) into reloaded script\n",
                        g_param_cache.size());
                }
            }

            // Restore persistent state into the new DLL — but drop it
            // when the schema versions disagree (and both sides
            // declared one), since set_state's silent default-fill on
            // a shape change would confuse the new code more than
            // starting fresh would.
            if (g_script.set_state && g_persistent_state_json.size() > 2) {
                int new_schema = g_script.state_schema_version
                               ? g_script.state_schema_version()
                               : 0;
                bool drop = (g_persistent_state_schema != 0 &&
                             new_schema != 0 &&
                             g_persistent_state_schema != new_schema);
                if (drop) {
                    std::fprintf(stderr,
                        "[xinsp2] state schema changed (v%d → v%d) — dropping prior state\n",
                        g_persistent_state_schema, new_schema);
                    std::string ev = "{\"type\":\"event\",\"name\":\"state_dropped\","
                                     "\"data\":{\"old_schema\":"
                                   + std::to_string(g_persistent_state_schema)
                                   + ",\"new_schema\":"
                                   + std::to_string(new_schema)
                                   + "}}";
                    srv.send_text(ev);
                    g_persistent_state_json = "{}";
                } else {
                    g_script.set_state(g_persistent_state_json.c_str());
                    std::fprintf(stderr, "[xinsp2] state restored (%zu bytes, schema v%d)\n",
                                 g_persistent_state_json.size(), new_schema);
                }
            }
        }

        // Build log can be large — send as a log message, not inline data.
        if (!res.build_log.empty()) {
            xp::LogMsg lm;
            lm.level = "info";
            lm.msg = res.build_log;
            srv.send_text(lm.to_json());
        }

        // Re-arm continuous mode if it was running before the reload,
        // at the same fps the original cmd:start asked for. The 4 s
        // cl.exe gap inside the reload is unavoidable; what we don't
        // want is the run staying dead afterwards and forcing the
        // caller to know they need to re-issue cmd:start.
        // Install the trigger sink so a source's emit_trigger runs an inspect
        // even when NOT continuous (single-shot) — issue/replay works without
        // cmd:start. Continuous mode (below) just adds the free-running timer.
        install_trigger_sink_(&srv);
        if (was_continuous) {
            // Preserve trigger-only mode across the reload: g_continuous_fps == 0
            // means no timer (sources drive it), so resume the same way.
            bool trig_only = prior_continuous_fps <= 0;
            int fps = trig_only ? 0 : prior_continuous_fps;
            g_continuous_fps = fps;
            g_continuous = true;
            int interval_ms = trig_only ? 0 : 1000 / std::max(fps, 1);
            int n_threads = g_plugin_mgr.project().dispatch_threads;
            if (grouping_enabled_()) spawn_group_pool_(&srv, interval_ms);
            else                     spawn_dispatch_pool_(&srv, interval_ms, n_threads);
            std::fprintf(stderr,
                "[xinsp2] continuous mode resumed after reload (%d threads)\n",
                n_threads);
        }

        // Return success with dll path + diagnostics (warnings only on
        // success; extension still wants them for squiggle).
        std::string data = "{\"dll\":";
        xp::json_escape_into(data, res.dll_path);
        data += ",\"diagnostics\":" + build_diag_json();
        if (was_continuous) data += ",\"resumed_continuous\":true";
        data += "}";
        send_rsp_ok(srv, id, data);
    } else if (name == "unload_script") {
        // P0-AB-1: dispatcher workers snapshot g_script under
        // g_script_mu and may be mid-inspect when unload_script
        // FreeLibrary's the DLL. Drain the pool first.
        (void)quiesce_dispatch_for_lifecycle_op_("unload_script");
        std::lock_guard<std::mutex> lk(g_script_mu);
        xi::script::unload_script(g_script);
        // Drop the param replay cache — there's no live script to
        // replay into, and a future load_project / compile_and_load
        // is free to start clean.
        g_param_cache.clear();
        send_rsp_ok(srv, id);
    } else if (name == "run") {
        if (g_continuous.load()) {
            send_rsp_err(srv, id, "cannot run while continuous mode is active — stop first");
            return;
        }
        int64_t run_id = ++g_run_id;

        // Optional `frame_path` arg — plumbed to the script via
        // `xi::current_frame_path()`. Was previously parsed by tests /
        // SDKs but ignored by this handler ("phantom argument"). Now
        // wired end to end.
        std::string frame_path;
        if (auto fp = xp::get_string_field(parsed->args_json, "frame_path")) {
            frame_path = *fp;
        }

        // Send rsp first (tests expect rsp before vars).
        char buf[128];
        std::snprintf(buf, sizeof(buf), R"({"run_id":%lld,"ms":0})", (long long)run_id);
        send_rsp_ok(srv, id, buf);

        // Run inspection on a detached thread so a long inspect doesn't block
        // the WS poll loop (and so the watchdog can observe its deadline slot).
        // Serialised on g_run_mu so 8 quick `cmd:run` calls produce
        // vars/history entries in run_id order.
        // SEH translator must be installed inside the thread.
        //
        // cmd:run is INTENTIONALLY serial — it's the deterministic single-shot
        // path (UI "Run", driver step-through) and is rejected outright while
        // continuous mode is active (above). Burst/throughput parallelism is the
        // continuous-mode dispatch pool's job (parallelism.dispatch_threads +
        // emit_trigger / fps); fanning out cmd:run would break this run_id-order
        // contract for no real burst gain (bursts arrive via the trigger path).
        crash_set(crash_ctx().last_cmd, sizeof(crash_ctx().last_cmd), "run");
        crash_ctx().last_run_id = (int)run_id;
        std::thread([&srv, run_id, frame_path = std::move(frame_path)]() {
            reserve_fault_stack();   // BUG 2: dump survives a script stack overflow
            _set_se_translator(seh_translator);
            std::lock_guard<std::mutex> lk(g_run_mu);
            run_one_inspection(srv, /*frame_hint=*/1, run_id, frame_path);
        }).detach();
    } else if (name == "start") {
        // Start continuous trigger mode. The backend runs a timer thread
        // that calls inspect() at a configurable interval. The script's
        // own ImageSource (if any) runs its acquisition thread inside
        // the DLL — the backend doesn't manage it.
        if (g_continuous.load()) {
            send_rsp_ok(srv, id, R"({"already":true})");
            return;
        }

        // Parse optional fps from args (default 10). fps <= 0 means TRIGGER-ONLY:
        // start continuous (spawn the lanes) but run NO synthetic timer tick — the
        // project's sources are the only dispatch driver. (Avoids loading the
        // default group with timer ticks; see docs/design/dispatch-groups.md.)
        int  fps = 10;
        bool trigger_only = false;
        auto fps_val = xp::get_number_field(parsed->args_json, "fps");
        if (fps_val) {
            if (*fps_val > 0) fps = (int)*fps_val;
            else trigger_only = true;
        }

        // Stop any existing pool before starting a new one.
        if (!g_worker_threads.empty() || g_timer_thread.joinable()) {
            stop_dispatch_pool_();
        }

        // A-P1-2: drain any events that arrived between the previous
        // cmd:stop and now (e.g. an in-flight emit_trigger that beat
        // clear_sink to the lock). Without this, the new run's first
        // batch of inspects fires on stale images from before the
        // user even called cmd:start, with cross-run image-handle
        // refs that the new sink would otherwise leak.
        {
            std::lock_guard<std::mutex> lk(g_ev_mu);
            for (auto& ev : g_ev_queue) {
                for (auto& [src, h] : ev.images) {
                    xi::ImagePool::instance().release(h);
                }
            }
            g_ev_queue.clear();
        }

        g_continuous_fps = trigger_only ? 0 : fps;
        g_continuous = true;
        // Reset queue stats so each cmd:start gets a fresh observation
        // window. Keeps `dispatch_stats` per-run comparable.
        g_dropped_oldest = 0;
        g_dropped_newest = 0;
        g_queue_high_watermark = 0;

        // interval_ms <= 0 → spawn_*_pool_ skips the timer thread (trigger-only).
        int interval_ms = trigger_only ? 0 : 1000 / std::max(fps, 1);
        int n_threads = g_plugin_mgr.project().dispatch_threads;
        if (n_threads < 1) n_threads = 1;

        // Bus-driven dispatch: with g_continuous now true the sink enqueues to
        // the worker pool (single-shot otherwise). Timer thread emits synthetic
        // events on schedule for scripts without trigger sources.
        install_trigger_sink_(&srv);
        if (grouping_enabled_()) spawn_group_pool_(&srv, interval_ms);
        else                     spawn_dispatch_pool_(&srv, interval_ms, n_threads);

        // The watchdog now tracks a per-inspect slot, so it protects every
        // worker under N>1 (no longer bypassed). On a hard trip the backend
        // exits for the FE to respawn; under N>1 the cooperative-cancel phase
        // is global (aborts all in-flight frames that round). See
        // run_one_inspection() + docs/guides/writing-a-script.md.

        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      R"({"started":true,"dispatch_threads":%d})", n_threads);
        send_rsp_ok(srv, id, buf);
    } else if (name == "stop") {
        g_continuous = false;
        g_ev_cv.notify_all();           // wake bus-driven worker
        // Force-release any breakpoint parking the worker — otherwise
        // join below would deadlock. breakpoint_cb also checks
        // g_continuous, so subsequent breakpoints in the same inspect()
        // no-op immediately.
        {
            std::lock_guard<std::mutex> lk(g_bp_mu);
            g_bp_paused = false;
        }
        g_bp_cv.notify_all();
        xi::TriggerBus::instance().clear_sink();
        stop_dispatch_pool_();
        // Drain any in-flight events that arrived between sink-clear and join.
        {
            std::lock_guard<std::mutex> lk(g_ev_mu);
            for (auto& ev : g_ev_queue) {
                for (auto& [src, h] : ev.images) xi::ImagePool::instance().release(h);
            }
            g_ev_queue.clear();
        }
        send_rsp_ok(srv, id, R"({"stopped":true})");
    } else if (name == "list_params") {
        // If a script is loaded, delegate to its own registry thunk so we
        // see the DLL's params. Otherwise report the backend's own.
        std::string params_json;
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.list_params) {
                std::vector<char> buf(64 * 1024);
                int n = g_script.list_params(buf.data(), (int)buf.size());
                if (n < 0) { buf.resize(static_cast<size_t>(-n) + 1024);
                             n = g_script.list_params(buf.data(), (int)buf.size()); }
                if (n > 0) params_json.assign(buf.data(), (size_t)n);
            }
        }
        if (params_json.empty()) {
            params_json = "[";
            auto list = xi::ParamRegistry::instance().list();
            for (size_t i = 0; i < list.size(); ++i) {
                if (i) params_json += ",";
                params_json += list[i]->as_json();
            }
            params_json += "]";
        }
        std::string out = "{\"type\":\"instances\",\"instances\":[],\"params\":";
        out += params_json;
        out += "}";
        send_rsp_ok(srv, id, "{}");
        srv.send_text(out);
    } else if (name == "set_param") {
        auto pname = xp::get_string_field(parsed->args_json, "name");
        if (!pname) {
            send_rsp_err(srv, id, "set_param: missing name");
            return;
        }
        // Try the loaded script first, then fall back to backend registry.
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.set_param) {
                // Extract raw value substring as a bare scalar.
                std::string val;
                auto num = xp::get_number_field(parsed->args_json, "value");
                if (num) { char nb[64]; std::snprintf(nb, sizeof(nb), "%g", *num); val = nb; }
                else {
                    if (parsed->args_json.find("\"value\":true")  != std::string::npos) val = "true";
                    if (parsed->args_json.find("\"value\":false") != std::string::npos) val = "false";
                }
                if (!val.empty()) {
                    int rc = g_script.set_param(pname->c_str(), val.c_str());
                    if (rc == 0) {
                        // Cache so compile_and_load can replay this
                        // value into the next DLL load (otherwise the
                        // new DLL's file-scope default would silently
                        // overwrite the user's tuned value).
                        g_param_cache[*pname] = val;
                        send_rsp_ok(srv, id);
                        return;
                    }
                    // fall through to backend registry on -1 (not found)
                }
            }
        }
        auto* p = xi::ParamRegistry::instance().find(*pname);
        if (!p) {
            send_rsp_err(srv, id, std::string("no such param: ") + *pname);
            return;
        }
        // Extract raw value substring from args_json. get_number_field
        // handles int/float; for bool we fall back to a string check.
        auto num = xp::get_number_field(parsed->args_json, "value");
        bool ok = false;
        if (num) {
            char nb[64];
            std::snprintf(nb, sizeof(nb), "%g", *num);
            ok = p->set_from_json(nb);
        } else {
            // Maybe "value":true / "value":false
            auto sv = xp::get_string_field(parsed->args_json, "value");
            if (sv) ok = p->set_from_json(*sv);
            else {
                if (parsed->args_json.find("\"value\":true")  != std::string::npos) ok = p->set_from_json("true");
                if (parsed->args_json.find("\"value\":false") != std::string::npos) ok = p->set_from_json("false");
            }
        }
        if (ok) send_rsp_ok(srv, id);
        else    send_rsp_err(srv, id, "set_param: bad value");
    } else if (name == "list_instances") {
        std::string inst_json, params_json;
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok()) {
                std::vector<char> buf(64 * 1024);
                if (g_script.list_instances) {
                    int n = g_script.list_instances(buf.data(), (int)buf.size());
                    if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024); n = g_script.list_instances(buf.data(), (int)buf.size()); }
                    if (n > 0) inst_json.assign(buf.data(), (size_t)n);
                }
                if (g_script.list_params) {
                    int n = g_script.list_params(buf.data(), (int)buf.size());
                    if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024); n = g_script.list_params(buf.data(), (int)buf.size()); }
                    if (n > 0) params_json.assign(buf.data(), (size_t)n);
                }
            }
        }
        // Also include backend-managed instances (from PluginManager)
        auto& proj = g_plugin_mgr.project();
        std::string backend_inst = "[";
        int bi = 0;
        for (auto& [k, v] : proj.instances) {
            if (bi++) backend_inst += ",";
            backend_inst += "{\"name\":\"" + v.name + "\",\"plugin\":\"" + v.plugin_name + "\"}";
        }
        backend_inst += "]";

        // Merge: script instances + backend instances
        std::string merged_inst;
        if (!inst_json.empty() && inst_json != "[]" && bi > 0) {
            // Both have entries — merge arrays
            merged_inst = inst_json.substr(0, inst_json.size() - 1) + "," + backend_inst.substr(1);
        } else if (bi > 0) {
            merged_inst = backend_inst;
        } else {
            merged_inst = inst_json.empty() ? "[]" : inst_json;
        }

        std::string out = "{\"type\":\"instances\",\"instances\":";
        out += merged_inst;
        out += ",\"params\":";
        out += params_json.empty() ? "[]" : params_json;
        out += "}";
        send_rsp_ok(srv, id, "{}");
        srv.send_text(out);
    } else if (name == "set_instance_def") {
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        // Extract the def object as a raw JSON substring
        std::string def_str;
        const char* after;
        if (xp::detail::find_key(parsed->args_json.data(),
                                  parsed->args_json.data() + parsed->args_json.size(),
                                  "def", def_str, after)) {
            // def_str is the raw JSON value
        } else {
            def_str = "{}";
        }
        // Try backend's InstanceRegistry first (plugin-manager instances)
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        if (inst) {
            if (inst->set_def(def_str)) send_rsp_ok(srv, id);
            else send_rsp_err(srv, id, "set_def returned false");
        } else {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.set_instance_def) {
                int rc = g_script.set_instance_def(iname->c_str(), def_str.c_str());
                if (rc == 0) send_rsp_ok(srv, id);
                else         send_rsp_err(srv, id, "set_instance_def failed");
            } else {
                send_rsp_err(srv, id, "instance not found: " + *iname);
            }
        }
    } else if (name == "exchange_instance") {
        // Crash-blame: capture which instance/plugin we're about to talk to.
        if (auto in = xp::get_string_field(parsed->args_json, "name")) {
            crash_set(crash_ctx().last_cmd, sizeof(crash_ctx().last_cmd), "exchange_instance");
            crash_set(crash_ctx().last_instance, sizeof(crash_ctx().last_instance), in->c_str());
            if (auto inst = xi::InstanceRegistry::instance().find(*in)) {
                crash_set(crash_ctx().last_plugin, sizeof(crash_ctx().last_plugin),
                          inst->plugin_name().c_str());
            }
        }
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        std::string cmd_str;
        const char* after;
        if (xp::detail::find_key(parsed->args_json.data(),
                                  parsed->args_json.data() + parsed->args_json.size(),
                                  "cmd", cmd_str, after)) {
        } else {
            cmd_str = "{}";
        }
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        if (inst) {
            try {
                std::string result = inst->exchange(cmd_str);
                send_rsp_ok(srv, id, result);
            } catch (const seh_exception& e) {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "exchange '%s' crashed: 0x%08X (%s)",
                             iname->c_str(), e.code, e.what());
                send_rsp_err(srv, id, msg);
            } catch (const std::exception& e) {
                send_rsp_err(srv, id, std::string("exchange error: ") + e.what());
            }
        } else {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.exchange_instance) {
                try {
                    std::vector<char> rsp(256 * 1024);
                    int n = g_script.exchange_instance(iname->c_str(), cmd_str.c_str(),
                                                       rsp.data(), (int)rsp.size());
                    if (n < 0) { rsp.resize((size_t)(-(int64_t)n) + 1024);
                                 n = g_script.exchange_instance(iname->c_str(), cmd_str.c_str(),
                                                                rsp.data(), (int)rsp.size()); }
                    if (n >= 0) send_rsp_ok(srv, id, std::string(rsp.data(), (size_t)n));
                    else        send_rsp_err(srv, id, "exchange_instance failed");
                } catch (const seh_exception& e) {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg), "script exchange '%s' crashed: 0x%08X (%s)",
                                 iname->c_str(), e.code, e.what());
                    send_rsp_err(srv, id, msg);
                }
            } else {
                send_rsp_err(srv, id, "instance not found: " + *iname);
            }
        }
    } else if (name == "save_project") {
        auto path = xp::get_string_field(parsed->args_json, "path");
        if (!path) { send_rsp_err(srv, id, "missing path"); return; }
        std::string params_json, inst_json;
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok()) {
                std::vector<char> buf(64 * 1024);
                if (g_script.list_params) {
                    int n = g_script.list_params(buf.data(), (int)buf.size());
                    if (n > 0) params_json.assign(buf.data(), (size_t)n);
                }
                if (g_script.list_instances) {
                    int n = g_script.list_instances(buf.data(), (int)buf.size());
                    if (n > 0) inst_json.assign(buf.data(), (size_t)n);
                }
            }
        }
        std::string content = xi::project::build_project_json(params_json, inst_json);
        if (xi::project::write_text(*path, content)) {
            send_rsp_ok(srv, id);
        } else {
            send_rsp_err(srv, id, "failed to write " + *path);
        }
    } else if (name == "commit_working_copy") {
        // Mirror the <project>/.xinsp_work scratch back onto the canonical
        // project — the UI "Save Project" action. Persist any live instance
        // configs to the scratch first so the commit captures them.
        for (auto& [iname, _] : g_plugin_mgr.project().instances) {
            g_plugin_mgr.save_instance(iname);
        }
        if (g_plugin_mgr.commit_working_copy()) {
            send_rsp_ok(srv, id, "{\"committed\":true,\"canonical\":" +
                        ([]{ std::string s; xp::json_escape_into(s, g_plugin_mgr.canonical_path()); return s; }()) + "}");
        } else {
            send_rsp_err(srv, id, "no working copy active (open with working_copy:true)");
        }
    } else if (name == "discard_working_copy") {
        // Blow away the scratch + re-seed from canonical, then reopen. Same
        // teardown constraint as open_project — drain the dispatch pool first.
        if (!g_plugin_mgr.has_working_copy()) {
            send_rsp_err(srv, id, "no working copy active");
            return;
        }
        (void)quiesce_dispatch_for_lifecycle_op_("discard_working_copy");
        if (g_plugin_mgr.reopen_fresh_working_copy()) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "discard failed");
        }
    } else if (name == "load_project") {
        auto path = xp::get_string_field(parsed->args_json, "path");
        if (!path) { send_rsp_err(srv, id, "missing path"); return; }
        std::string content = xi::project::read_text(*path);
        if (content.empty()) { send_rsp_err(srv, id, "failed to read " + *path); return; }

        // Use cJSON to parse the project file properly
        cJSON* root = cJSON_Parse(content.c_str());
        if (!root) { send_rsp_err(srv, id, "invalid JSON in project file"); return; }

        // Restore params
        cJSON* params = cJSON_GetObjectItem(root, "params");
        if (params && cJSON_IsArray(params)) {
            int n = cJSON_GetArraySize(params);
            for (int i = 0; i < n; ++i) {
                cJSON* item = cJSON_GetArrayItem(params, i);
                cJSON* nm = cJSON_GetObjectItem(item, "name");
                cJSON* val = cJSON_GetObjectItem(item, "value");
                if (nm && cJSON_IsString(nm) && val) {
                    char vbuf[64] = {};
                    if (cJSON_IsNumber(val))     std::snprintf(vbuf, sizeof(vbuf), "%g", val->valuedouble);
                    else if (cJSON_IsBool(val))  std::snprintf(vbuf, sizeof(vbuf), "%s", cJSON_IsTrue(val) ? "true" : "false");
                    else continue;
                    // Try script params first, then backend params
                    std::lock_guard<std::mutex> lk(g_script_mu);
                    if (g_script.ok() && g_script.set_param) {
                        g_script.set_param(nm->valuestring, vbuf);
                    } else {
                        auto* p = xi::ParamRegistry::instance().find(nm->valuestring);
                        if (p) p->set_from_json(vbuf);
                    }
                }
            }
        }

        // Restore instance configs
        cJSON* instances = cJSON_GetObjectItem(root, "instances");
        if (instances && cJSON_IsArray(instances)) {
            int n = cJSON_GetArraySize(instances);
            for (int i = 0; i < n; ++i) {
                cJSON* item = cJSON_GetArrayItem(instances, i);
                cJSON* nm = cJSON_GetObjectItem(item, "name");
                cJSON* def = cJSON_GetObjectItem(item, "def");
                if (nm && cJSON_IsString(nm) && def) {
                    char* def_str = cJSON_PrintUnformatted(def);
                    auto inst = xi::InstanceRegistry::instance().find(nm->valuestring);
                    if (inst) inst->set_def(def_str);
                    cJSON_free(def_str);
                }
            }
        }

        cJSON_Delete(root);
        send_rsp_ok(srv, id);
    } else if (name == "preview_instance") {
        // Grab the latest frame from a named ImageSource instance,
        // JPEG-encode it, and send as a binary preview frame.
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        auto* src = inst ? dynamic_cast<xi::ImageSource*>(inst.get()) : nullptr;
        if (!src) { send_rsp_err(srv, id, "not an ImageSource: " + *iname); return; }

        xi::Image img = src->grab();
        if (img.empty()) { send_rsp_ok(srv, id, R"({"frame":false})"); return; }

        std::vector<uint8_t> jpeg;
        if (!xi::encode_jpeg(img, 80, jpeg)) { send_rsp_ok(srv, id, R"({"frame":false})"); return; }

        // Use a hash of the instance name as gid so the extension can
        // route the preview to the correct panel.
        uint32_t preview_gid = 9000;
        for (char c : *iname) preview_gid = preview_gid * 31 + (uint8_t)c;
        preview_gid = 9000 + (preview_gid % 1000);

        char gid_buf[64];
        std::snprintf(gid_buf, sizeof(gid_buf), R"({"frame":true,"gid":%u})", preview_gid);
        send_rsp_ok(srv, id, gid_buf);

        std::vector<uint8_t> frame(xp::kPreviewHeaderSize + jpeg.size());
        xp::PreviewHeader hd;
        hd.gid = preview_gid;
        hd.codec = (uint32_t)xp::Codec::JPEG;
        hd.width = (uint32_t)img.width;
        hd.height = (uint32_t)img.height;
        hd.channels = (uint32_t)img.channels;
        xp::encode_preview_header(hd, frame.data());
        std::memcpy(frame.data() + xp::kPreviewHeaderSize, jpeg.data(), jpeg.size());
        srv.send_binary(frame.data(), frame.size());
    } else if (name == "process_instance") {
        // Call a C ABI plugin's process() with an image from another instance.
        // args: { name: "detector0", source: "cam0", params: {...} }
        auto iname = xp::get_string_field(parsed->args_json, "name");
        auto source = xp::get_string_field(parsed->args_json, "source");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        crash_set(crash_ctx().last_cmd, sizeof(crash_ctx().last_cmd), "process_instance");
        crash_set(crash_ctx().last_instance, sizeof(crash_ctx().last_instance), iname->c_str());

        // Find the plugin instance
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        auto* adapter = inst ? dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get()) : nullptr;
        if (!adapter || !adapter->process_fn()) {
            send_rsp_err(srv, id, "not a processable plugin: " + *iname);
            return;
        }

        // Get source image from an ImageSource instance
        xi::Image src_img;
        if (source) {
            auto src_inst = xi::InstanceRegistry::instance().find(*source);
            auto* img_src = src_inst ? dynamic_cast<xi::ImageSource*>(src_inst.get()) : nullptr;
            if (img_src) src_img = img_src->grab();
        }
        if (src_img.empty()) {
            send_rsp_err(srv, id, "no image available — provide a source instance or start streaming");
            return;
        }

        // Build input record with the image
        static xi_host_api host = xi::ImagePool::make_host_api();
        xi_image_handle src_h = xi::ImagePool::instance().from_image(src_img);

        // Parse extra params from args
        std::string params_json = "{}";
        const char* after;
        if (xp::detail::find_key(parsed->args_json.data(),
                                  parsed->args_json.data() + parsed->args_json.size(),
                                  "params", params_json, after)) {
            // params_json is already set
        }

        xi_record_image in_imgs[] = { {"gray", src_h} };
        xi_record input_rec;
        input_rec.images = in_imgs;
        input_rec.image_count = 1;
        input_rec.json = params_json.c_str();

        xi_record_out output;
        xi_record_out_init(&output);

        try {
            adapter->process_fn()(adapter->raw_instance(), &input_rec, &output);
        } catch (const seh_exception& e) {
            xi::ImagePool::instance().release(src_h);
            xi_record_out_free(&output);
            char msg[256];
            std::snprintf(msg, sizeof(msg), "process_instance '%s' crashed: 0x%08X (%s)",
                         iname->c_str(), e.code, e.what());
            send_rsp_err(srv, id, msg);
            return;
        } catch (const std::exception& e) {
            xi::ImagePool::instance().release(src_h);
            xi_record_out_free(&output);
            send_rsp_err(srv, id, std::string("process_instance '") + *iname +
                                  "' threw: " + e.what());
            return;
        } catch (...) {
            xi::ImagePool::instance().release(src_h);
            xi_record_out_free(&output);
            send_rsp_err(srv, id, "process_instance '" + *iname + "' threw unknown exception");
            return;
        }

        // Release input image handle (success path)
        xi::ImagePool::instance().release(src_h);

        // Build response: JSON data + send output images as preview frames
        std::string result_json = output.json ? output.json : "{}";

        // Add image info to result
        std::string full_json = result_json;
        if (output.image_count > 0) {
            // Insert images info into the JSON
            if (full_json.size() > 1 && full_json.back() == '}') {
                full_json.pop_back();
                if (full_json.size() > 1) full_json += ",";
                full_json += "\"_images\":{";
                for (int i = 0; i < output.image_count; ++i) {
                    if (i) full_json += ",";
                    uint32_t gid = 8000 + (uint32_t)i;
                    full_json += "\"" + std::string(output.images[i].key) + "\":" + std::to_string(gid);
                }
                full_json += "}}";
            }
        }

        send_rsp_ok(srv, id, full_json);

        // Send output images as binary preview frames
        for (int i = 0; i < output.image_count; ++i) {
            auto& oi = output.images[i];
            xi::Image out_img = xi::ImagePool::instance().to_image(oi.handle);
            // Always release the output handle — regardless of encode success
            xi::ImagePool::instance().release(oi.handle);

            if (out_img.empty()) continue;

            xi::Image jpeg_img = out_img;
            if (out_img.channels == 1) {
                jpeg_img = xi::Image(out_img.width, out_img.height, 3);
                for (int j = 0; j < out_img.width * out_img.height; ++j) {
                    jpeg_img.data()[j*3+0] = out_img.data()[j];
                    jpeg_img.data()[j*3+1] = out_img.data()[j];
                    jpeg_img.data()[j*3+2] = out_img.data()[j];
                }
            }

            std::vector<uint8_t> jpeg;
            if (!xi::encode_jpeg(jpeg_img, 85, jpeg)) continue;

            uint32_t gid = 8000 + (uint32_t)i;
            std::vector<uint8_t> frame(xp::kPreviewHeaderSize + jpeg.size());
            xp::PreviewHeader hd;
            hd.gid = gid;
            hd.codec = (uint32_t)xp::Codec::JPEG;
            hd.width = (uint32_t)out_img.width;
            hd.height = (uint32_t)out_img.height;
            hd.channels = (uint32_t)jpeg_img.channels;
            xp::encode_preview_header(hd, frame.data());
            std::memcpy(frame.data() + xp::kPreviewHeaderSize, jpeg.data(), jpeg.size());
            srv.send_binary(frame.data(), frame.size());
        }

        xi_record_out_free(&output);
    } else if (name == "list_plugins") {
        auto plugins = g_plugin_mgr.list_plugins();
        std::string out = "[";
        auto esc = [](const std::string& s) {
            std::string o; for (char c : s) { if (c=='\\'||c=='"') o.push_back('\\'); o.push_back(c); } return o;
        };
        for (size_t i = 0; i < plugins.size(); ++i) {
            if (i) out += ",";
            auto& p = plugins[i];
            out += "{\"name\":\"" + esc(p.name) + "\",\"description\":\"" + esc(p.description) + "\"";
            out += ",\"folder\":\"" + esc(p.folder_path) + "\"";
            out += ",\"has_ui\":" + std::string(p.has_ui ? "true" : "false");
            out += ",\"loaded\":" + std::string(p.handle ? "true" : "false");
            // Same origin field as to_json — extension's pluginTree relies
            // on it to badge project plugins, e2e journey asserts it.
            bool is_proj = g_plugin_mgr.is_project_plugin(p.name);
            out += ",\"origin\":\"" + std::string(is_proj ? "project" : "global") + "\"";
            // Cert snapshot (doesn't re-run the tests — just reads cert.json if present)
            xi::cert::Cert c;
            if (xi::cert::read(p.folder_path, c)) {
                auto dll_path = std::filesystem::path(p.folder_path) / p.dll_name;
                bool valid = xi::cert::is_valid(p.folder_path, dll_path);
                out += ",\"cert\":{\"present\":true,\"valid\":" + std::string(valid ? "true" : "false");
                out += ",\"baseline_version\":" + std::to_string(c.baseline_version);
                out += ",\"certified_at\":\"" + esc(c.certified_at) + "\"}";
            } else {
                out += ",\"cert\":{\"present\":false}";
            }
            // Optional `manifest` block from plugin.json (free-form;
            // see docs/reference/plugin-abi.md). AI agents and doc
            // tools read this to discover params / inputs / outputs /
            // exchange surface without grepping plugin source.
            if (!p.manifest_json.empty()) {
                out += ",\"manifest\":" + p.manifest_json;
            }
            out += "}";
        }
        out += "]";
        send_rsp_ok(srv, id, out);
    } else if (name == "recent_errors") {
        // Return error events captured by the cross-channel ring
        // (rsp.error / log level=error / event:isolation_dead etc).
        // Optional `since_ms` arg filters out older entries — useful
        // for "any errors since I sent my last cmd?" polling.
        auto since_opt = xp::get_number_field(parsed->args_json, "since_ms");
        int64_t since = since_opt ? (int64_t)*since_opt : 0;
        std::string out = "[";
        {
            std::lock_guard<std::mutex> lk(g_recent_errors_mu);
            int n = 0;
            for (auto& e : g_recent_errors) {
                if (e.ts_ms < since) continue;
                if (n++) out += ",";
                out += "{\"ts_ms\":" + std::to_string(e.ts_ms);
                out += ",\"source\":"; xp::json_escape_into(out, e.source);
                out += ",\"message\":"; xp::json_escape_into(out, e.message);
                if (e.cmd_id) out += ",\"cmd_id\":" + std::to_string(e.cmd_id);
                if (e.run_id) out += ",\"run_id\":" + std::to_string(e.run_id);
                out += "}";
            }
        }
        out += "]";
        send_rsp_ok(srv, id, out);
    } else if (name == "status") {
        // Snapshot of every component's latest sticky status. Clients call this
        // on EVERY (re)connect — that re-pull over the retained map is what
        // guarantees the latest status always arrives, even across reconnects
        // and backend respawns; the `status` push event is just a low-latency
        // accelerator between snapshots.
        std::string out = "{";
        {
            std::lock_guard<std::mutex> lk(g_status_mu);
            int n = 0;
            for (auto& [who, e] : g_status) {
                if (n++) out += ",";
                xp::json_escape_into(out, who);
                out += ":{\"text\":"; xp::json_escape_into(out, e.text);
                out += ",\"ts_ms\":" + std::to_string(e.ts_ms);
                out += ",\"seq\":" + std::to_string(e.seq) + "}";
            }
        }
        out += "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "image_pool_stats") {
        // Per-owner ImagePool footprint. Owner IDs alone are
        // meaningless to humans — we look them up against the
        // running project's instances + script so the reply names
        // who is holding the memory. Anonymous (owner == 0) is
        // collapsed under "label":"<host>".
        auto totals   = xi::ImagePool::instance().stats();
        auto by_owner = xi::ImagePool::instance().stats_by_owner();

        // Build owner_id → label map.
        std::unordered_map<xi::ImagePoolOwnerId, std::string> labels;
        labels[0] = "<host>";
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.owner_id != 0) {
                labels[g_script.owner_id] =
                    "script:" + std::filesystem::path(g_script.path).filename().string();
            }
        }
        for (auto& [iname, ii] : g_plugin_mgr.project().instances) {
            if (auto* a = dynamic_cast<xi::CAbiInstanceAdapter*>(ii.instance.get())) {
                labels[a->owner_id()] = "instance:" + ii.name + " (" + ii.plugin_name + ")";
            }
            // ProcessInstanceAdapter owns handles in the worker's pool,
            // not the host's — they don't show up here. SHM-backed
            // handles also don't show up in the host ImagePool stats.
        }

        auto label_for = [&](xi::ImagePoolOwnerId o) -> std::string {
            auto it = labels.find(o);
            if (it != labels.end()) return it->second;
            return "owner:" + std::to_string(o) + " (orphan)";
        };

        // Cumulative diagnostics: total_created and high_water never
        // decrement, so they expose activity even when live counts are
        // zero between runs (the agent feedback loop hit this — live
        // snapshots looked empty mid-test, hiding real allocation).
        auto cum = xi::ImagePool::instance().cumulative();
        std::string out = "{\"total\":{\"handles\":"
                        + std::to_string(totals.handle_count)
                        + ",\"bytes\":"
                        + std::to_string(totals.total_bytes)
                        + "},\"cumulative\":{\"total_created\":"
                        + std::to_string(cum.total_created)
                        + ",\"high_water\":"
                        + std::to_string(cum.high_water)
                        + ",\"live_now\":"
                        + std::to_string(cum.live_now)
                        + "},\"by_owner\":[";
        for (size_t i = 0; i < by_owner.size(); ++i) {
            if (i) out += ",";
            out += "{\"owner\":"   + std::to_string(by_owner[i].owner)
                +  ",\"label\":";
            xp::json_escape_into(out, label_for(by_owner[i].owner));
            out +=  ",\"handles\":" + std::to_string(by_owner[i].handle_count)
                +  ",\"bytes\":"    + std::to_string(by_owner[i].total_bytes)
                +  "}";
        }
        out += "]}";
        send_rsp_ok(srv, id, out);
    } else if (name == "rescan_plugins") {
        // Optional arg: {"dir": "<path>"} scans that one dir (additive).
        // No arg: re-scan the default plugins_dir.
        auto dir_opt = xp::get_string_field(parsed->args_json, "dir");
        const std::string& dir = dir_opt ? *dir_opt : g_plugins_dir;
        int n = 0;
        if (!dir.empty() && std::filesystem::exists(dir)) {
            n = g_plugin_mgr.scan_plugins(dir);
        }
        std::string out = "{\"scanned\":";
        xp::json_escape_into(out, dir);
        out += ",\"count\":" + std::to_string(n) + "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "load_plugin") {
        auto pname = xp::get_string_field(parsed->args_json, "name");
        if (!pname) { send_rsp_err(srv, id, "missing name"); return; }
        if (g_plugin_mgr.load_plugin(*pname)) {
            send_rsp_ok(srv, id);
        } else {
            send_rsp_err(srv, id, "failed to load plugin: " + *pname);
        }
    } else if (name == "create_project") {
        auto folder = xp::get_string_field(parsed->args_json, "folder");
        auto pname  = xp::get_string_field(parsed->args_json, "name");
        if (!folder || !pname) { send_rsp_err(srv, id, "missing folder or name"); return; }
        if (g_plugin_mgr.create_project(*folder, *pname)) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "failed to create project");
        }
    } else if (name == "open_project") {
        // Accept either `folder` (historical) or `path` (matches what
        // the protocol doc + Python SDK / load_project use). Same arg,
        // different name; this defuses the inconsistency the AI agent
        // hit on the size-buckets case.
        auto folder = xp::get_string_field(parsed->args_json, "folder");
        if (!folder) folder = xp::get_string_field(parsed->args_json, "path");
        if (!folder) { send_rsp_err(srv, id, "missing folder/path"); return; }
        // P0-AB-3: must drain dispatch pool BEFORE the old project is
        // torn down. open_project (the PluginManager method) destroys
        // the previous project's instances and FreeLibrary's its
        // plugin DLLs; if a worker is mid-inspect into a now-freed
        // plugin function we SEGV.
        // working_copy: operate on a <project>/.xinsp_work scratch copy
        // (resume if present, else seed) so edits are transactional + crash-
        // durable. Default false = legacy in-place behaviour.
        bool working_copy = parsed->args_json.find("\"working_copy\":true") != std::string::npos
                          || parsed->args_json.find("\"working_copy\": true") != std::string::npos;
        (void)quiesce_dispatch_for_lifecycle_op_("open_project");
        if (g_plugin_mgr.open_project(*folder, working_copy)) {
            auto& proj = g_plugin_mgr.project();
            int inst_count = (int)proj.instances.size();
            std::fprintf(stderr, "[xinsp2] project opened: %s (%d instances)\n",
                         proj.name.c_str(), inst_count);
            for (auto& [k, v] : proj.instances) {
                std::fprintf(stderr, "[xinsp2]   instance: %s (%s)\n",
                             k.c_str(), v.plugin_name.c_str());
            }
            // Surface skip-bad-instance warnings to the user. The project
            // open still succeeds; bad instances are simply absent from
            // the runtime registry. Extension can show a toast.
            auto warns = g_plugin_mgr.open_warnings();
            if (!warns.empty()) {
                std::string s = "project opened with " + std::to_string(warns.size())
                              + " skipped instance(s):";
                for (auto& w : warns) {
                    s += "\n  - " + w.instance;
                    if (!w.plugin.empty()) s += " (" + w.plugin + ")";
                    s += ": " + w.reason;
                }
                xp::LogMsg lm;
                lm.level = "warn";
                lm.msg = s;
                srv.send_text(lm.to_json());
            }
            // Remember the canonical folder + apply any per-project toolchain
            // override (project.json "toolchain" block) so the compiler and the
            // IntelliSense config below both pick up the user's path fixes.
            g_project_folder = *folder;
            resolve_toolchain_(*folder);
            // Put the project folder on the DLL search path so a script's
            // statically-linked external dep DLL can live in the project folder.
            set_project_dll_search_(*folder);
            // Drop a c_cpp_properties.json into the project the user actually
            // edits (the canonical *folder, not any .xinsp_work scratch) so the
            // C/C++ extension resolves xi/* + OpenCV and go-to-definition works
            // with no false red squiggles. Best-effort; never blocks the open.
            write_cpp_intellisense_config_(*folder);
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "failed to open project in " + *folder);
        }
    } else if (name == "close_project") {
        // P0-AB-3: must drain dispatch pool BEFORE close_project tears
        // down instances and FreeLibrary's plugin DLLs. (PR #33 fixed
        // the in-PluginManager teardown order; this fixes the
        // dispatcher-still-running case the dispatcher pool hit when
        // close_project is sent during continuous mode.)
        (void)quiesce_dispatch_for_lifecycle_op_("close_project");
        g_plugin_mgr.close_project();
        send_rsp_ok(srv, id, "{\"closed\":true}");
    } else if (name == "export_project_plugin") {
        // Package a project plugin as a deployable folder. Compiles
        // Release + runs baseline cert; on success, the destination
        // contains a self-contained plugin.json + DLL + cert.json that
        // can be dropped into another project's plugins folder.
        auto pname = xp::get_string_field(parsed->args_json, "plugin");
        auto dest  = xp::get_string_field(parsed->args_json, "dest");
        if (!pname || !dest) { send_rsp_err(srv, id, "missing plugin or dest"); return; }
        if (!g_plugin_mgr.is_project_plugin(*pname)) {
            send_rsp_err(srv, id, "not a project plugin: " + *pname);
            return;
        }
        // P0-AB-3: export_project_plugin runs the plugin under cert,
        // which loads + invokes its DLL. Make sure no dispatcher
        // worker is mid-call into the same plugin's instances.
        (void)quiesce_dispatch_for_lifecycle_op_("export_project_plugin");
        auto er = g_plugin_mgr.export_project_plugin(*pname, *dest);
        std::string data = "{\"plugin\":";
        xp::json_escape_into(data, *pname);
        data += ",\"dest\":";
        xp::json_escape_into(data, er.dest_dir);
        data += ",\"cert_passed\":" + std::string(er.cert_passed ? "true" : "false");
        data += ",\"cert_pass_count\":" + std::to_string(er.cert_pass_count);
        data += ",\"cert_fail_count\":" + std::to_string(er.cert_fail_count);
        data += "}";
        if (er.ok) {
            send_rsp_ok(srv, id, data);
        } else {
            xp::Rsp r;
            r.id = id;
            r.ok = false;
            r.error = er.error;
            r.data_json = data;
            srv.send_text(r.to_json());
            if (!er.build_log.empty()) {
                xp::LogMsg lm;
                lm.level = "error";
                lm.msg = er.build_log;
                srv.send_text(lm.to_json());
            }
        }
    } else if (name == "recompile_project_plugin") {
        // Hot-rebuild a single project-local plugin. The extension calls
        // this from a file watcher when the user edits plugin source.
        // On success the plugin's instances are re-instantiated with
        // their previous defs intact; on failure the old DLL stays
        // loaded so running inspection isn't disrupted.
        auto pname = xp::get_string_field(parsed->args_json, "plugin");
        if (!pname) { send_rsp_err(srv, id, "missing plugin"); return; }
        if (!g_plugin_mgr.is_project_plugin(*pname)) {
            send_rsp_err(srv, id, "not a project plugin: " + *pname);
            return;
        }
        // P0-AB-4: recompile resets each instance pointer then
        // FreeLibrary's the old DLL. Any in-flight set_def / exchange
        // on those instances from a dispatcher worker would dereference
        // freed code. Drain first.
        auto guard = quiesce_dispatch_for_lifecycle_op_("recompile_project_plugin");
        auto rr = g_plugin_mgr.recompile_project_plugin(*pname);
        // Build diagnostics JSON — same shape as compile_and_load.
        std::string diag_json = "[";
        for (size_t i = 0; i < rr.diagnostics.size(); ++i) {
            auto& d = rr.diagnostics[i];
            if (i) diag_json += ",";
            diag_json += "{\"file\":";  xp::json_escape_into(diag_json, d.file);
            diag_json += ",\"line\":" + std::to_string(d.line);
            diag_json += ",\"col\":"  + std::to_string(d.col);
            diag_json += ",\"severity\":"; xp::json_escape_into(diag_json, d.severity);
            diag_json += ",\"code\":";    xp::json_escape_into(diag_json, d.code);
            diag_json += ",\"message\":"; xp::json_escape_into(diag_json, d.message);
            diag_json += "}";
        }
        diag_json += "]";
        std::string data = "{\"plugin\":";
        xp::json_escape_into(data, *pname);
        data += ",\"diagnostics\":" + diag_json;
        data += ",\"reattached\":[";
        for (size_t i = 0; i < rr.reattached_instances.size(); ++i) {
            if (i) data += ",";
            xp::json_escape_into(data, rr.reattached_instances[i]);
        }
        data += "]}";
        if (rr.ok) {
            send_rsp_ok(srv, id, data);
        } else {
            xp::Rsp r;
            r.id = id;
            r.ok = false;
            r.error = rr.error;
            r.data_json = data;
            srv.send_text(r.to_json());
            if (!rr.build_log.empty()) {
                xp::LogMsg lm;
                lm.level = "error";
                lm.msg = rr.build_log;
                srv.send_text(lm.to_json());
            }
        }
    } else if (name == "recording_start") {
        auto folder = xp::get_string_field(parsed->args_json, "folder");
        if (!folder) { send_rsp_err(srv, id, "missing folder"); return; }
        if (xi::TriggerRecorder::instance().start(*folder)) {
            std::string out = "{\"recording\":true,\"folder\":";
            xp::json_escape_into(out, *folder);
            out += "}";
            send_rsp_ok(srv, id, out);
        } else {
            send_rsp_err(srv, id, "already recording");
        }
    } else if (name == "recording_stop") {
        bool ok = xi::TriggerRecorder::instance().stop();
        std::string out = "{\"recording\":false,\"events\":" +
            std::to_string(xi::TriggerRecorder::instance().event_count()) + "}";
        send_rsp_ok(srv, id, out);
        (void)ok;
    } else if (name == "recording_status") {
        std::string out = "{\"recording\":";
        out += xi::TriggerRecorder::instance().is_recording() ? "true" : "false";
        out += ",\"replaying\":";
        out += xi::TriggerRecorder::instance().is_replaying() ? "true" : "false";
        out += ",\"events\":" + std::to_string(xi::TriggerRecorder::instance().event_count());
        out += ",\"folder\":";
        xp::json_escape_into(out, xi::TriggerRecorder::instance().folder());
        out += "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "recording_replay") {
        auto folder = xp::get_string_field(parsed->args_json, "folder");
        if (!folder) { send_rsp_err(srv, id, "missing folder"); return; }
        auto speed = xp::get_number_field(parsed->args_json, "speed").value_or(1.0);
        if (xi::TriggerRecorder::instance().start_replay(*folder, speed)) {
            send_rsp_ok(srv, id, "{\"started\":true}");
        } else {
            send_rsp_err(srv, id, "could not start replay (no manifest, or already replaying)");
        }
    } else if (name == "dispatch_stats") {
        // Snapshot of queue health. Useful for drivers / agents that
        // want to know if their source is overproducing.
        //
        // Field semantics:
        //   queue_depth_now             — current queue size
        //   queue_depth_cap             — configured project.json cap
        //   queue_depth_high_watermark  — peak depth observed since
        //                                 last cmd:start (real obs.)
        //   dropped_oldest / dropped_newest — overflow counters since
        //                                 last cmd:start
        //
        // ALL THREE COUNTERS (high_watermark, dropped_oldest,
        // dropped_newest) are reset by cmd:start. Drivers that snapshot
        // before AND after a start will see the AFTER values come back
        // smaller than BEFORE — don't subtract. See docs/protocol.md
        // `dispatch_stats` for the public contract.
        std::string data;
        size_t qsz;
        { std::lock_guard<std::mutex> lk(g_ev_mu); qsz = g_ev_queue.size(); }
        data  = "{\"queue_depth_now\":" + std::to_string(qsz);
        data += ",\"queue_depth_cap\":" + std::to_string(g_plugin_mgr.project().queue_depth);
        data += ",\"queue_depth_high_watermark\":" + std::to_string(g_queue_high_watermark.load());
        data += ",\"overflow\":\"" + g_plugin_mgr.project().overflow + "\"";
        data += ",\"dispatch_threads\":" + std::to_string(g_plugin_mgr.project().dispatch_threads);
        data += ",\"dropped_oldest\":" + std::to_string(g_dropped_oldest.load());
        data += ",\"dropped_newest\":" + std::to_string(g_dropped_newest.load());
        // Per-group lanes (only when dispatch groups are active). Snapshot the
        // lane list under g_lanes_mu so a concurrent stop can't free them mid-read.
        std::vector<std::shared_ptr<GroupLane>> lanes;
        { std::lock_guard<std::mutex> lk(g_lanes_mu); lanes = g_lanes; }
        if (!lanes.empty()) {
            data += ",\"groups\":[";
            for (size_t i = 0; i < lanes.size(); ++i) {
                auto& l = *lanes[i];
                size_t lq; { std::lock_guard<std::mutex> lk(l.mu); lq = l.q.size(); }
                if (i) data += ",";
                data += "{\"name\":"; xp::json_escape_into(data, l.cfg.name);
                data += ",\"max_parallel\":" + std::to_string(l.cfg.max_parallel);
                data += ",\"thread_priority\":\"" + l.cfg.thread_priority + "\"";
                data += ",\"queue_now\":" + std::to_string(lq);
                data += ",\"running\":" + std::to_string(l.running.load());
                data += ",\"high_watermark\":" + std::to_string(l.high_watermark.load());
                data += ",\"dropped\":" + std::to_string(l.dropped.load());
                data += ",\"cpu_affinity\":[";   // [] = unbound; else the per-worker mask sets
                for (size_t si = 0; si < l.cfg.cpu_affinity.size(); ++si) {
                    if (si) data += ",";
                    data += "[";
                    for (size_t ci = 0; ci < l.cfg.cpu_affinity[si].size(); ++ci) {
                        if (ci) data += ",";
                        data += std::to_string(l.cfg.cpu_affinity[si][ci]);
                    }
                    data += "]";
                }
                data += "]}";
            }
            data += "]";
        }
        data += "}";
        send_rsp_ok(srv, id, data);
    } else if (name == "open_project_warnings") {
        // Returns the per-instance warnings collected during the most
        // recent open_project. open_project itself succeeds even when
        // individual instances fail (skip-bad-instance), so this is
        // how a UI / agent surfaces those problems instead of having
        // to scrape backend stderr.
        auto warnings = g_plugin_mgr.open_warnings();
        std::string data = "{\"warnings\":[";
        bool first = true;
        for (auto& w : warnings) {
            if (!first) data += ",";
            first = false;
            data += "{\"instance\":";
            xp::json_escape_into(data, w.instance);
            data += ",\"plugin\":";
            xp::json_escape_into(data, w.plugin);
            data += ",\"reason\":";
            xp::json_escape_into(data, w.reason);
            data += "}";
        }
        data += "]}";
        send_rsp_ok(srv, id, data);
    } else if (name == "set_trigger_policy") {
        // args: { policy: "any"|"all_required"|"leader_followers",
        //         required: ["cam_left", ...],
        //         leader: "cam_left",
        //         window_ms: 100 }
        auto pol_str = xp::get_string_field(parsed->args_json, "policy");
        xi::TriggerPolicy pol = xi::TriggerPolicy::Any;
        if      (pol_str && *pol_str == "all_required")     pol = xi::TriggerPolicy::AllRequired;
        else if (pol_str && *pol_str == "leader_followers") pol = xi::TriggerPolicy::LeaderFollowers;
        // Parse `required` properly (cJSON, not substring). The old
        // substring scan looked for `"required":[` (no space) and
        // silently fell back to an empty list when the args came from
        // Python's default `json.dumps(...)` which emits `"required":
        // [` (with space). The empty list then got persisted to
        // project.json by save_project_locked() — silent destruction
        // of the user's policy.
        std::vector<std::string> required;
        if (cJSON* root = cJSON_Parse(parsed->args_json.c_str())) {
            if (cJSON* arr = cJSON_GetObjectItem(root, "required");
                arr && cJSON_IsArray(arr)) {
                cJSON* it;
                cJSON_ArrayForEach(it, arr) {
                    if (cJSON_IsString(it) && it->valuestring) {
                        required.emplace_back(it->valuestring);
                    }
                }
            }
            cJSON_Delete(root);
        }
        auto leader = xp::get_string_field(parsed->args_json, "leader").value_or("");
        auto win    = xp::get_number_field(parsed->args_json, "window_ms").value_or(100);
        if (g_plugin_mgr.set_trigger_policy(pol, required, leader, (int)win)) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "no project open");
        }
    } else if (name == "recertify_plugin") {
        auto pname = xp::get_string_field(parsed->args_json, "name");
        if (!pname) { send_rsp_err(srv, id, "missing name"); return; }
        auto* pi = g_plugin_mgr.find_plugin(*pname);
        if (!pi) { send_rsp_err(srv, id, "unknown plugin: " + *pname); return; }
        // Delete existing cert so load_plugin re-runs baseline on next scan.
        auto cert_path = std::filesystem::path(pi->folder_path) / "cert.json";
        std::error_code ec;
        std::filesystem::remove(cert_path, ec);
        // If currently loaded, run baseline now and write cert in-place.
        if (pi->handle) {
            auto syms = xi::baseline::load_symbols(pi->handle);
            static xi_host_api host = xi::ImagePool::make_host_api();
            auto summary = xi::cert::certify(pi->folder_path,
                std::filesystem::path(pi->folder_path) / pi->dll_name,
                pi->name, syms, &host);
            std::string rsp_json = "{\"passed\":" + std::string(summary.all_passed ? "true" : "false");
            rsp_json += ",\"pass_count\":" + std::to_string(summary.pass_count);
            rsp_json += ",\"fail_count\":" + std::to_string(summary.fail_count);
            rsp_json += ",\"total_ms\":" + std::to_string(summary.total_ms);
            rsp_json += ",\"failures\":[";
            bool first = true;
            for (auto& r : summary.results) {
                if (!r.passed) {
                    if (!first) rsp_json += ",";
                    first = false;
                    auto esc = [](const std::string& s) {
                        std::string o; for (char c : s) { if (c=='\\'||c=='"') o.push_back('\\'); o.push_back(c); } return o;
                    };
                    rsp_json += "{\"name\":\"" + esc(r.name) + "\",\"error\":\"" + esc(r.error) + "\"}";
                }
            }
            rsp_json += "]}";
            send_rsp_ok(srv, id, rsp_json);
        } else {
            send_rsp_ok(srv, id, "{\"queued\":true,\"note\":\"will re-cert on next load\"}");
        }
    } else if (name == "create_instance") {
        auto iname  = xp::get_string_field(parsed->args_json, "name");
        auto plugin = xp::get_string_field(parsed->args_json, "plugin");
        if (!iname || !plugin) { send_rsp_err(srv, id, "missing name or plugin"); return; }
        // Ensure plugin is loaded — surface WHY if it can't be (missing DLL,
        // failed cert, ABI mismatch, etc.) instead of a generic failure.
        std::string load_err;
        if (!g_plugin_mgr.load_plugin(*plugin, &load_err)) {
            send_rsp_err(srv, id, load_err.empty() ? "failed to load plugin" : load_err);
            return;
        }
        std::string create_err;
        auto* ii = g_plugin_mgr.create_instance(*iname, *plugin, &create_err);
        if (ii) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, create_err.empty() ? "failed to create instance" : create_err);
        }
    } else if (name == "remove_instance") {
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        bool delete_folder =
            parsed->args_json.find("\"delete_folder\":true") != std::string::npos;
        if (g_plugin_mgr.remove_instance(*iname, delete_folder)) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "instance not found: " + *iname);
        }
    } else if (name == "rename_instance") {
        auto old_name = xp::get_string_field(parsed->args_json, "name");
        auto new_name = xp::get_string_field(parsed->args_json, "new_name");
        if (!old_name || !new_name) { send_rsp_err(srv, id, "missing name or new_name"); return; }
        if (g_plugin_mgr.rename_instance(*old_name, *new_name)) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "rename failed — name in use or instance missing");
        }
    } else if (name == "get_project") {
        send_rsp_ok(srv, id, g_plugin_mgr.to_json());
    } else if (name == "save_instance_config") {
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        if (g_plugin_mgr.save_instance(*iname)) {
            send_rsp_ok(srv, id);
        } else {
            send_rsp_err(srv, id, "instance not found: " + *iname);
        }
    } else if (name == "get_plugin_ui") {
        // Return the path to the plugin's UI folder so the extension can
        // load it into a webview.
        auto plugin = xp::get_string_field(parsed->args_json, "plugin");
        if (!plugin) { send_rsp_err(srv, id, "missing plugin"); return; }
        auto* pi = g_plugin_mgr.find_plugin(*plugin);
        if (pi && pi->has_ui) {
            std::string data = "{\"ui_path\":";
            xp::json_escape_into(data, pi->ui_path);
            data += "}";
            send_rsp_ok(srv, id, data);
        } else {
            send_rsp_err(srv, id, "no UI for plugin: " + *plugin);
        }
    } else if (name == "toolchain_health") {
        // C++ toolchain health check for the open project. Reports each
        // component's resolved path + source (override/env/default/none) +
        // whether it exists, so the config UI can warn on missing/wrong paths.
        send_rsp_ok(srv, id, toolchain_health_json_(g_project_folder));
    } else if (name == "set_toolchain_override") {
        // Pin (or clear) one toolchain path in the project's project.json
        // "toolchain" block. args: { key: "opencv"|"turbojpeg"|"ipp"|"vcvars"|
        // "include", path: "<dir-or-file>" }  (empty path clears the override).
        // Takes effect on the next compile; we also re-resolve + regenerate the
        // IntelliSense config immediately so the editor updates.
        auto key  = xp::get_string_field(parsed->args_json, "key");
        auto path = xp::get_string_field(parsed->args_json, "path");
        if (!key) { send_rsp_err(srv, id, "missing key"); return; }
        // map UI key -> project.json field name
        std::string field;
        if      (*key == "include")   field = "include_dir";
        else if (*key == "opencv")    field = "opencv_dir";
        else if (*key == "turbojpeg") field = "turbojpeg_root";
        else if (*key == "ipp")       field = "ipp_root";
        else if (*key == "vcvars")    field = "vcvars";
        else { send_rsp_err(srv, id, "unknown toolchain key: " + *key); return; }
        std::string err;
        if (!write_toolchain_override_(g_project_folder, field, path ? *path : std::string(), err)) {
            send_rsp_err(srv, id, "set_toolchain_override failed: " + err);
            return;
        }
        // Re-resolve globals + refresh the generated IntelliSense config so both
        // the next compile and the editor reflect the change without a reopen.
        resolve_toolchain_(g_project_folder);
        write_cpp_intellisense_config_(g_project_folder);
        std::string data = "{\"applied\":true,\"recompile_needed\":true,\"health\":";
        data += toolchain_health_json_(g_project_folder);
        data += "}";
        send_rsp_ok(srv, id, data);
    } else {
        send_rsp_err(srv, id, std::string("unknown command: ") + name);
    }
}

// Map an instruction pointer to "<module>+0x<offset>" by scanning
// loaded modules. Used in the crash filter to point at which DLL
// (script vs plugin vs xinsp-backend itself) was executing.
static std::string blame_module(void* addr) {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return "<unknown>";
    int n = (int)(needed / sizeof(HMODULE));
    for (int i = 0; i < n; ++i) {
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        auto base = (uintptr_t)mi.lpBaseOfDll;
        if ((uintptr_t)addr < base || (uintptr_t)addr >= base + mi.SizeOfImage) continue;
        char name[MAX_PATH];
        GetModuleFileNameA(mods[i], name, sizeof(name));
        const char* slash = std::strrchr(name, '\\');
        std::string out = (slash ? slash + 1 : name);
        char off[64];
        std::snprintf(off, sizeof(off), "+0x%llx", (unsigned long long)((uintptr_t)addr - base));
        return out + off;
    }
    return "<unknown>";
}

// JSON-escape a path segment in-place (writes into out). Tiny copy of
// xp::json_escape_into to keep this filter free of any nontrivial dep.
static void crash_json_escape(std::string& out, const char* s) {
    out.push_back('"');
    for (; *s; ++s) {
        char c = *s;
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out.push_back(c);
        }
    }
    out.push_back('"');
}

static const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:        return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:          return "STACK_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:      return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:      return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:   return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_ILLEGAL_INSTRUCTION:     return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:        return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:           return "IN_PAGE_ERROR";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:return "NONCONTINUABLE";
        case 0xE06D7363:                        return "MS_C++_EXCEPTION";
        // Synthetic codes we RaiseException with so write_minidump runs for
        // CRT death paths that bypass SEH (terminate/abort/fastfail family).
        case 0xE0000001:                        return "TEST_CRASH";
        case 0xE0000002:                        return "CXX_TERMINATE";
        case 0xE0000003:                        return "CXX_ABORT";
        case 0xE0000004:                        return "CRT_INVALID_PARAMETER";
        case 0xE0000005:                        return "CXX_PURE_CALL";
        default:                                return "UNKNOWN";
    }
}

// Reserve stack headroom so the unhandled-exception filter (write_minidump) can
// still run after a STACK_OVERFLOW — otherwise the filter has no stack left and
// the process dies with NO minidump/sidecar (robustness BUG 2). Call once at the
// top of every thread that runs untrusted inspect/plugin code.
static void reserve_fault_stack() {
#ifdef _WIN32
    ULONG guarantee = 128 * 1024;  // 128 KB — room for the filter + MiniDumpWriteDump
    SetThreadStackGuarantee(&guarantee);
#endif
}

// Top-level unhandled-exception filter. Writes a minidump under
// %TEMP%/xinsp2/crashdumps PLUS a sibling .json crash report containing
// exception kind, faulting module, and the last activity context. The
// report is read by the backend on the NEXT startup and surfaced via
// cmd:crash_reports — the extension shows it as a notification so the
// user knows *which* component (script / plugin / core) caused the
// last session's death.
static LONG WINAPI write_minidump(EXCEPTION_POINTERS* info) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "xinsp2" / "crashdumps";
    std::error_code ec;
    fs::create_directories(dir, ec);
    SYSTEMTIME st; GetLocalTime(&st);
    char stem[128];
    std::snprintf(stem, sizeof(stem),
        "xinsp-backend-%lu-%04d%02d%02d-%02d%02d%02d",
        (unsigned long)GetCurrentProcessId(),
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    auto dmp_path  = (dir / (std::string(stem) + ".dmp")).string();
    auto json_path = (dir / (std::string(stem) + ".json")).string();

    DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    void* addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
    std::string blamed = blame_module(addr);

    // 1. Minidump
    HANDLE h = CreateFileA(dmp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers    = FALSE;
        // Richer than MiniDumpNormal so the dump is self-contained for
        // post-mortem of an in-process compute-core crash:
        //   WithDataSegs              — globals (breadcrumb table,
        //                               recent_errors ring) land in the dump
        //   WithThreadInfo            — per-thread times / teb
        //   WithIndirectlyReferenced  — pointee memory of stack locals
        //                               (e.g. the TriggerEvent being inspected)
        //   WithUnloadedModules       — a just-FreeLibrary'd plugin still
        //                               shows in the module list for blame
        // Deliberately NOT WithFullMemory — large image buffers would
        // bloat the dump to GBs; the above captures the forensic state
        // without the bulk.
        auto dump_type = (MINIDUMP_TYPE)(
            MiniDumpNormal
            | MiniDumpWithDataSegs
            | MiniDumpWithThreadInfo
            | MiniDumpWithIndirectlyReferencedMemory
            | MiniDumpWithUnloadedModules);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                          dump_type, &mei, nullptr, nullptr);
        CloseHandle(h);
    }

    // 2. JSON sidecar — what the next-startup report path reads.
    std::string out = "{\"version\":\""  XINSP2_VERSION "\""
                      ",\"commit\":\""  XINSP2_COMMIT "\""
                      ",\"pid\":" + std::to_string(GetCurrentProcessId())
                    + ",\"thread_id\":" + std::to_string(GetCurrentThreadId());
    char tsbuf[64];
    std::snprintf(tsbuf, sizeof(tsbuf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    out += ",\"timestamp\":";
    crash_json_escape(out, tsbuf);
    out += ",\"exception\":{\"code\":";
    char codebuf[24];
    std::snprintf(codebuf, sizeof(codebuf), "\"0x%08X\"", code);
    out += codebuf;
    out += ",\"name\":";
    crash_json_escape(out, exception_name(code));
    char addrbuf[40];
    std::snprintf(addrbuf, sizeof(addrbuf), "\"0x%llx\"", (unsigned long long)addr);
    out += ",\"address\":"; out += addrbuf;
    out += ",\"module\":"; crash_json_escape(out, blamed.c_str());
    out += "}";
    // `context` = the faulting thread's breadcrumb (the handler runs on
    // the faulting thread, so crash_ctx() is its slot). Back-compat
    // with the existing report reader which expects this object.
    uint32_t fault_tid = (uint32_t)GetCurrentThreadId();
    {
        auto& c = crash_ctx();
        out += ",\"context\":{";
        out += "\"last_cmd\":";      crash_json_escape(out, c.last_cmd);
        out += ",\"last_script\":";  crash_json_escape(out, c.last_script);
        out += ",\"last_instance\":";crash_json_escape(out, c.last_instance);
        out += ",\"last_plugin\":";  crash_json_escape(out, c.last_plugin);
        out += ",\"last_phase\":";   crash_json_escape(out, c.last_phase);
        out += ",\"last_status\":";  crash_json_escape(out, c.last_status);
        out += ",\"last_run_id\":" + std::to_string(c.last_run_id);
        out += ",\"last_frame\":"  + std::to_string(c.last_frame);
        out += "}";
    }
    // `threads` = every claimed breadcrumb slot, so a multi-dispatch
    // crash shows what ALL concurrent inspects were doing, not just
    // the faulting one. `faulting:true` flags the culprit.
    out += ",\"threads\":[";
    {
        bool first = true;
        for (int i = 0; i < kMaxCrashSlots; ++i) {
            uint32_t tid = g_crash_slot_tid[i].load(std::memory_order_acquire);
            if (tid == 0) continue;
            auto& c = g_crash_slots[i];
            if (!first) out += ",";
            first = false;
            out += "{\"thread_id\":" + std::to_string(tid);
            out += ",\"faulting\":" + std::string(tid == fault_tid ? "true" : "false");
            out += ",\"last_cmd\":";     crash_json_escape(out, c.last_cmd);
            out += ",\"last_instance\":";crash_json_escape(out, c.last_instance);
            out += ",\"last_plugin\":";  crash_json_escape(out, c.last_plugin);
            out += ",\"last_phase\":";   crash_json_escape(out, c.last_phase);
            out += ",\"last_status\":";  crash_json_escape(out, c.last_status);
            out += ",\"last_run_id\":" + std::to_string(c.last_run_id);
            out += ",\"last_frame\":"  + std::to_string(c.last_frame);
            out += "}";
        }
    }
    out += "]";
    out += ",\"minidump\":";
    crash_json_escape(out, (std::string(stem) + ".dmp").c_str());
    out += "}\n";

    HANDLE jh = CreateFileA(json_path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (jh != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(jh, out.data(), (DWORD)out.size(), &wrote, nullptr);
        CloseHandle(jh);
    }

    std::fprintf(stderr, "[xinsp2] CRASH 0x%08X (%s) in %s — minidump: %s\n",
                 code, exception_name(code), blamed.c_str(), dmp_path.c_str());
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

// std::terminate handler — fires when an unhandled C++ exception
// unwinds out of a thread (e.g. a detached worker thread that didn't
// wrap its body in try/catch). This path bypasses
// SetUnhandledExceptionFilter on its own, so a silent terminate would
// produce no crashdump. We:
//   1. Log the current exception's what()/type so the cause appears
//      in stderr (and thus the bash exit summary).
//   2. RaiseException with a recognisable code so write_minidump
//      sees a thread context and can write the dump + json sidecar.
[[noreturn]] static void on_terminate() noexcept {
    const char* what  = "<no exception>";
    const char* tname = "<no exception>";
    try {
        if (auto p = std::current_exception()) std::rethrow_exception(p);
    } catch (const std::exception& e) {
        what  = e.what();
        tname = typeid(e).name();
    } catch (const seh_exception& e) {
        what  = e.what();
        tname = "xi::seh_exception";
    } catch (...) {
        tname = "<non-std exception>";
    }
    std::fprintf(stderr,
        "[xinsp2] std::terminate (thread %lu): %s — %s\n",
        (unsigned long)GetCurrentThreadId(), tname, what);
    std::fflush(stderr);
    crash_set(crash_ctx().last_cmd, sizeof(crash_ctx().last_cmd), "terminate");
    // 0xE0000002 — distinct from --test-crash's 0xE0000001 so blame_module
    // and exception_name still tag it as MS_C++ish; the json_path will
    // record this code so the next-startup report distinguishes the
    // two paths. NONCONTINUABLE so the filter actually runs.
    RaiseException(0xE0000002, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::abort();   // unreachable; quiets [[noreturn]]
}

// CRT abort()/fastfail family — std::abort(), a failed C `assert`, a CRT
// invalid-parameter trip, or a pure-virtual call all terminate the process via
// __fastfail (0xC0000409), which bypasses BOTH SetUnhandledExceptionFilter
// (write_minidump) AND std::set_terminate (on_terminate). Without a handler a
// script that calls abort() kills the backend leaving NO minidump / .json
// sidecar — defeating cmd:crash_reports AND the FE crash-history / status
// channel (their forensics come from that sidecar). We intercept each entry
// point and re-raise a NONCONTINUABLE exception so write_minidump runs with a
// real thread context (same trick as on_terminate). Robustness BUG 1, found by
// the robustness-fuzzer dogfood; see docs/design/fe-be-split.md crash story.
[[noreturn]] static void raise_for_dump(const char* cause, DWORD code) noexcept {
    crash_set(crash_ctx().last_cmd, sizeof(crash_ctx().last_cmd), cause);
    std::fprintf(stderr, "[xinsp2] CRT fatal (%s) — writing crash report\n", cause);
    std::fflush(stderr);
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::abort();   // unreachable; quiets [[noreturn]]
}
static void on_sigabrt(int) { raise_for_dump("abort", 0xE0000003); }
static void on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*,
                                 unsigned int, uintptr_t) {
    raise_for_dump("invalid_parameter", 0xE0000004);
}
static void on_purecall() { raise_for_dump("purecall", 0xE0000005); }

// Vectored exception handler — runs BEFORE SEH translators, before
// any per-thread try/__except. Logs first-chance exceptions that
// might get swallowed silently. Returning EXCEPTION_CONTINUE_SEARCH
// lets normal handling proceed; we're just listening here.
//
// Filtered to the codes that would actually kill the process if
// unhandled: AVs, illegal instructions, stack overflow, fastfail,
// our own RaiseException codes. Skipping benign first-chance C++
// exceptions (0xE06D7363) that happen all the time during normal
// try/catch flow.
static LONG WINAPI veh_logger(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = info->ExceptionRecord->ExceptionCode;
    // Whitelist things that are actually concerning. C++ exceptions
    // (0xE06D7363) and breakpoints get filtered out.
    bool concerning =
        code == EXCEPTION_ACCESS_VIOLATION ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_NONCONTINUABLE_EXCEPTION ||
        code == 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN / fastfail */ ||
        code == 0xC0000374 /* STATUS_HEAP_CORRUPTION */ ||
        (code >= 0xE0000001 && code <= 0xE0000010);
    if (concerning) {
        void* addr = info->ExceptionRecord->ExceptionAddress;
        std::string blamed = blame_module(addr);
        std::fprintf(stderr,
            "[xinsp2] VEH first-chance 0x%08X (%s) thread %lu at %s\n",
            code, exception_name(code),
            (unsigned long)GetCurrentThreadId(), blamed.c_str());
        std::fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv) {
    // Top-level guard: minidump on crashes that escape the SEH translator
    // (stack overflow, plugin static destructor faults, etc.).
    SetUnhandledExceptionFilter(write_minidump);
    // Install SEH → C++ exception translator so try/catch catches segfaults
    _set_se_translator(seh_translator);
    // C++ terminate path — covers unhandled exceptions in detached threads
    // (the silent-exit pattern the spike branch's process-isolation work
    // hit during validation).
    std::set_terminate(on_terminate);
    // Vectored handler — first crack at every concerning exception, even
    // ones that get suppressed somewhere downstream. Diagnostic only;
    // doesn't change the exception's normal handling path.
    AddVectoredExceptionHandler(/*first=*/1, veh_logger);
    // CRT fastfail family (abort / failed assert / invalid-parameter / pure
    // call) bypasses the three handlers above. Catch each so a crash report is
    // ALWAYS written (robustness BUG 1). SIGABRT must have a handler installed
    // BEFORE any abort(); _set_abort_behavior clears the popup + the Watson/
    // fastfail report so our handler is the path that runs.
    std::signal(SIGABRT, on_sigabrt);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(on_invalid_parameter);
    _set_purecall_handler(on_purecall);
    reserve_fault_stack();   // BUG 2: let the filter dump on a main-thread stack overflow
    // Tell Windows not to silently kill us on heap corruption — we want
    // to see crashpad's report instead. (HeapEnableTerminationOnCorruption
    // is opt-IN; HeapDisableCoalesceOnFree is unrelated. The default in
    // newer Windows versions IS termination-on-corruption; flipping it
    // off via SetProcessDEPPolicy isn't needed — just ensure we get the
    // event.)

    // --test-crash: deliberately trigger a fatal exception so the
    // top-level minidump filter fires. Used by runCrashDump E2E.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--test-crash") {
            RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
            return 99;  // unreachable
        }
        // --test-abort: exercise the CRT abort() path (robustness BUG 1) — must
        // produce the same minidump + .json sidecar as a real exception crash.
        if (std::string_view(argv[i]) == "--test-abort") {
            std::abort();
            return 99;  // unreachable
        }
        // --test-stackoverflow: exercise the stack-overflow path (robustness BUG 2)
        // — with reserve_fault_stack() the filter must still write a dump+sidecar.
        if (std::string_view(argv[i]) == "--test-stackoverflow") {
            reserve_fault_stack();
            // Unbounded recursion with a volatile sink the optimizer can't elide.
            struct Boom { static int go(volatile int x) { return x + go(x + 1) + go(x + 2); } };
            return Boom::go(1);  // unreachable — overflows the stack
        }
    }

    // --version / -v / --help / -h short-circuit before any side effects.
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--version" || a == "-v") {
            std::printf("xinsp-backend %s (%s)\n", XINSP2_VERSION, XINSP2_COMMIT);
            return 0;
        }
        if (a == "--help" || a == "-h") {
            std::printf(
                "xinsp-backend %s — xInsp2 inspection server\n"
                "\n"
                "Usage: xinsp-backend [options]\n"
                "  --port=N             WebSocket port (default 7823)\n"
                "  --host=ADDR          bind address (default 127.0.0.1; use 0.0.0.0 for remote)\n"
                "  --auth=SECRET        require Bearer SECRET in handshake\n"
                "  --plugins-dir=DIR    extra plugin folder (repeatable)\n"
                "  --watchdog=MS        per-inspect budget: cooperative-cancel, then exit\n"
                "                       for FE respawn if ignored (default 0 = off)\n"
                "  --project=DIR        headless autostart: open this project at boot\n"
                "  --script=PATH        script to compile for --project (default: project.json's)\n"
                "  --autostart-fps=N    with --project, start continuous mode at N fps (0 = off)\n"
                "  --working-copy       edit a <project>/.xinsp_work scratch copy (transactional;\n"
                "                       resumes on crash respawn). commit_working_copy to save\n"
                "  --comms-port=N       connect to the comms gateway on loopback N (xi::comms)\n"
                "  --priority=CLASS     process priority: high|above|normal|below|realtime (Win)\n"
                "  --version, -v        print version and exit\n"
                "  --help, -h           this help\n",
                XINSP2_VERSION);
            return 0;
        }
    }

    int port = parse_port(argc, argv);

    // ---- thread/process performance knobs --------------------------------------
#ifdef _WIN32
    // Raise the OS timer resolution to 1ms (default ~15.6ms) so timer-tick fps,
    // sleeps, and CV waits are tight. winmm.timeBeginPeriod via runtime-load so we
    // don't add a link dependency. Process-wide; the paired timeEndPeriod is optional.
    if (HMODULE w = LoadLibraryA("winmm.dll")) {
        if (auto fn = (UINT(WINAPI*)(UINT))GetProcAddress(w, "timeBeginPeriod")) fn(1);
    }
    // --priority=<class>: bump the whole backend's process priority (for a
    // dedicated inspection PC). Default = leave as-is. "realtime" can starve the
    // OS — use with care.
    if (std::string pri = parse_str_flag(argc, argv, "--priority"); !pri.empty()) {
        DWORD cls = 0;
        if      (pri == "high")     cls = HIGH_PRIORITY_CLASS;
        else if (pri == "above")    cls = ABOVE_NORMAL_PRIORITY_CLASS;
        else if (pri == "normal")   cls = NORMAL_PRIORITY_CLASS;
        else if (pri == "below")    cls = BELOW_NORMAL_PRIORITY_CLASS;
        else if (pri == "realtime") cls = REALTIME_PRIORITY_CLASS;
        if (cls) { SetPriorityClass(GetCurrentProcess(), cls);
            std::fprintf(stderr, "[xinsp2] process priority = %s\n", pri.c_str()); }
        else std::fprintf(stderr,
            "[xinsp2] unknown --priority '%s' (high|above|normal|below|realtime)\n", pri.c_str());
    }
#else
    // TODO(linux): clock_nanosleep is already high-res; setpriority(PRIO_PROCESS)
    // / sched_setscheduler for --priority.
#endif

    // Derive include dir for the script compiler. In a normal dev tree the
    // backend .exe is at backend/build/Release, and headers are at
    // backend/include. Walk up until we find xi/xi.hpp.
    {
        std::filesystem::path p = get_exe_dir();
        for (int i = 0; i < 6; ++i) {
            if (std::filesystem::exists(p / "include" / "xi" / "xi.hpp")) {
                g_include_dir = (p / "include").string();
                break;
            }
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
        if (g_include_dir.empty()) {
            // Fallback: next to the exe.
            g_include_dir = (std::filesystem::path(get_exe_dir()) / "include").string();
        }
        // Remember the shipped headers as the default a project override falls
        // back to (see resolve_toolchain_).
        g_include_dir_default = g_include_dir;
    }
    g_work_dir = (std::filesystem::temp_directory_path() / "xinsp2").string();
    std::filesystem::create_directories(g_work_dir);

    // Probe accelerators once. Logged so the user can see what their
    // compiled scripts will inherit.
    g_opencv_dir     = xi::script::detail::probe_opencv_dir();
    g_turbojpeg_root = xi::script::detail::probe_turbojpeg_root();
    g_ipp_root       = xi::script::detail::probe_ipp_root();
    std::fprintf(stderr, "[xinsp2] script-side accelerators: opencv=%s  turbojpeg=%s  ipp=%s\n",
                 g_opencv_dir.empty()     ? "no" : g_opencv_dir.c_str(),
                 g_turbojpeg_root.empty() ? "no" : g_turbojpeg_root.c_str(),
                 g_ipp_root.empty()       ? "no" : g_ipp_root.c_str());

    // Find and scan plugins directory (sibling of backend/)
    {
        std::filesystem::path p = get_exe_dir();
        for (int i = 0; i < 6; ++i) {
            if (std::filesystem::exists(p / "plugins")) {
                g_plugins_dir = (p / "plugins").string();
                break;
            }
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
    }
    if (!g_plugins_dir.empty()) {
        int n = g_plugin_mgr.scan_plugins(g_plugins_dir);
        std::fprintf(stderr, "[xinsp2] scanned %d plugins from %s\n", n, g_plugins_dir.c_str());
    }
    // Additional plugin folders from --plugins-dir / XINSP2_EXTRA_PLUGIN_DIRS.
    // Lets external SDKs keep their plugin DLLs in place — no copy needed.
    for (auto& dir : parse_extra_plugin_dirs(argc, argv)) {
        if (!std::filesystem::exists(dir)) {
            std::fprintf(stderr, "[xinsp2] extra plugin dir not found: %s\n", dir.c_str());
            continue;
        }
        int n = g_plugin_mgr.scan_plugins(dir);
        std::fprintf(stderr, "[xinsp2] scanned %d plugins from %s\n", n, dir.c_str());
    }

    std::fprintf(stderr, "[xinsp2] include_dir=%s\n", g_include_dir.c_str());
    std::fprintf(stderr, "[xinsp2] work_dir=%s\n",    g_work_dir.c_str());
    std::fprintf(stderr, "[xinsp2] plugins_dir=%s\n",  g_plugins_dir.c_str());

    // Process isolation + SHM removed 2026-05: all plugins run
    // in-process and share the host ImagePool directly (zero-copy via
    // pointers, no cross-process marshalling). No worker process, no
    // shared-memory region to set up.

    // Hand the same compile environment that xi::script::compile uses
    // to the plugin manager — project plugins (compiled when a project
    // is opened) need the include dir, vcvars, and accelerator roots.
    xi::CompileEnv env;
    env.include_dir    = g_include_dir;
    env.opencv_dir     = g_opencv_dir;
    env.turbojpeg_root = g_turbojpeg_root;
    env.ipp_root       = g_ipp_root;
    g_plugin_mgr.set_compile_env(env);

    xi::ws::Server srv;
    srv.on_open  = [&] {
        std::fprintf(stderr, "[xinsp2] client connected\n");
        send_hello(srv);
    };
    srv.on_close = [&] {
        std::fprintf(stderr, "[xinsp2] client disconnected\n");
        // E-P1-2: a fresh client should get a fresh server view.
        // Without these clears the next driver to reconnect inherits
        // the prior session's subscription set, history ring, error
        // ring, and "isolation_dead already reported" memo. None of
        // that is visible from the new client's perspective and at
        // best confuses, at worst hides regressions (an instance that
        // dies AGAIN under the new client would silently be unreported
        // because the dedup set still contains its name).
        {
            std::lock_guard<std::mutex> lk(g_sub_mu);
            g_sub_all = true;
            g_sub_names.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_hist_mu);
            g_history.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_recent_errors_mu);
            g_recent_errors.clear();
        }
        // E-P1-1: clear the dedup set so a re-dying instance is
        // re-reported to the next client.
    };
    srv.on_text = [&](std::string_view s) {
        handle_command(srv, s);
    };
    srv.on_binary = [&](const uint8_t*, size_t n) {
        std::fprintf(stderr, "[xinsp2] unexpected binary frame: %zu bytes\n", n);
    };

    std::string host   = parse_host(argc, argv);
    std::string secret = parse_auth_secret(argc, argv);
    srv.set_bind_host(host);
    if (!secret.empty()) srv.set_auth_secret(secret);

    g_watchdog_ms = parse_watchdog_ms(argc, argv);
    if (g_watchdog_ms.load() > 0) {
        std::fprintf(stderr, "[xinsp2] watchdog enabled: %d ms per inspect\n", g_watchdog_ms.load());
    }
    g_srv_for_bp = &srv;   // S3: breakpoint_cb emits events through it
    // Route plugin host_api->set_status into the status registry. Non-capturing
    // so it converts to the StatusSinkFn function pointer.
    xi::status_sink() = [](const char* who, const char* text) {
        set_status_internal((who && *who) ? who : "@plugin", text);
    };

    // --comms-port=N: connect to the out-of-process comms gateway (xinsp-comms)
    // on loopback so scripts can use xi::comms::* for PLC I/O. Tolerates the FE
    // spawn race (retries). Stays null if not configured -> xi::comms is no-op.
    static GatewayClient g_gw_instance;
    if (std::string cp = parse_str_flag(argc, argv, "--comms-port"); !cp.empty()) {
        int port = 0; try { port = std::stoi(cp); } catch (...) {}
        if (port > 0 && g_gw_instance.connect(port)) {
            g_gateway = &g_gw_instance;
            std::fprintf(stderr, "[xinsp2] comms gateway connected (loopback:%d)\n", port);
        } else {
            std::fprintf(stderr, "[xinsp2] comms gateway NOT connected (port %s) — "
                         "xi::comms will be inert\n", cp.c_str());
        }
    }
    // P2.4 watchdog. Always-on monitor thread; acts when any in-flight inspect
    // (wd_arm slot) overruns its deadline. Two-phase, now per-worker-aware:
    //   Phase 1 — cooperative: set the script's GLOBAL cancel flag; xi::ops poll
    //     xi::cancellation_requested() and bail. 1000 ms grace (big ops — 20 MP
    //     gaussian, matchTemplate, contour walks — need a few hundred ms to
    //     finish their current chunk; 100 ms tripped healthy scripts). Under N>1
    //     the flag is global, so it aborts every in-flight frame this round —
    //     the intended "something's wedged, bail" signal; healthy workers re-run
    //     next tick.
    //   Phase 2 — hard trip: if any slot is STILL overrun after the grace, the
    //     script ignored cooperative cancel. We do NOT TerminateThread — a kill
    //     mid process() would leak the per-instance lock (deadlocking that
    //     instance) and risk heap corruption. The process is unrecoverable, so
    //     the backend EXITS; the FE supervisor respawns a clean one (and drives
    //     the line safe). Run without an FE => backend stays down by design.
    g_watchdog_run = true;
    g_watchdog_thread = std::thread([&srv]() {
        auto now_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        while (g_watchdog_run.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!wd_any_overran(now_ms())) continue;

            // Phase 1: cooperative cancel + grace.
            {
                std::lock_guard<std::mutex> lk(g_script_mu);
                if (g_script.set_global_cancel) g_script.set_global_cancel(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            // Did every overrun inspect return (slot freed / re-armed fresh)?
            if (!wd_any_overran(now_ms())) {
                {
                    std::lock_guard<std::mutex> lk(g_script_mu);
                    if (g_script.set_global_cancel) g_script.set_global_cancel(0);
                }
                int n = ++g_watchdog_trips;
                std::fprintf(stderr,
                    "[xinsp2] watchdog tripped (#%d) — script honoured cooperative cancel\n", n);
                emit_error_log(srv,
                    "watchdog tripped — inspect exceeded "
                    + std::to_string(g_watchdog_ms.load())
                    + "ms; cooperative cancel succeeded");
                continue;
            }

            // Phase 2: hard trip — exit for FE respawn (see header above).
            ++g_watchdog_trips;
            std::fprintf(stderr,
                "[xinsp2] watchdog HARD trip - inspect exceeded %dms and ignored "
                "cooperative cancel; exiting for supervisor respawn (rc=0x%04X)\n",
                g_watchdog_ms.load(), WATCHDOG_EXIT_CODE);
            emit_error_log(srv,
                "watchdog HARD trip — inspect exceeded "
                + std::to_string(g_watchdog_ms.load())
                + "ms and ignored cooperative cancel; backend exiting for respawn");
            std::fflush(stderr);
            std::fflush(stdout);
            // _Exit: skip static destructors / atexit — a wedged worker may hold
            // locks those would block on. The FE sees the exit and respawns.
            std::_Exit(WATCHDOG_EXIT_CODE);
        }
    });
    if (!srv.start(port)) {
        std::fprintf(stderr, "[xinsp2] failed to bind %s:%d\n", host.c_str(), port);
        return 1;
    }
    if (host == "0.0.0.0" && secret.empty()) {
        std::fprintf(stderr,
            "[xinsp2] WARNING: bound to 0.0.0.0 with NO --auth secret — anyone reachable can drive the backend\n");
    }
    std::fprintf(stderr, "[xinsp2] listening on ws://%s:%d%s\n",
                 host.c_str(), port,
                 secret.empty() ? "" : " (auth required)");
    std::fflush(stderr);

    // Headless autostart (--project). Drives the same WS command handlers a
    // client would call — open_project, then compile_and_load, then (optional)
    // start — by synthesizing wire-format frames and feeding handle_command.
    // No client need ever connect: reply sends are no-ops while srv has no
    // client (xi_ws_server send_frame returns false at INVALID_SOCK). The WS
    // port stays open so an operator HMI / the VS Code extension can attach
    // live. Used by xinsp-fe.exe to run a line headlessly.
    if (std::string project = parse_str_flag(argc, argv, "--project"); !project.empty()) {
        std::string script = parse_str_flag(argc, argv, "--script");
        int autostart_fps  = parse_autostart_fps(argc, argv);
        // --working-copy: open via a <project>/.xinsp_work scratch. On a crash
        // respawn the FE passes the same flag; the scratch still exists, so the
        // backend resumes the last in-progress settings instead of reverting to
        // the pristine project. See docs/guides/project-working-copy.md.
        bool working_copy = has_flag(argc, argv, "--working-copy");

        bool degraded = false;
        // Validate the project BEFORE opening. A nonexistent / project.json-less
        // dir can't inspect, so don't let it reach "ready" and be reported
        // healthy (an operator typo would otherwise yield a green, blind line).
        std::error_code proj_ec;
        if (!std::filesystem::exists(std::filesystem::path(project) / "project.json", proj_ec)) {
            std::fprintf(stderr, "[xinsp2] autostart: degraded - no project.json at %s; "
                         "cannot run (not reporting ready)\n", project.c_str());
            degraded = true;
        } else {
            std::fprintf(stderr, "[xinsp2] autostart: open_project %s%s\n", project.c_str(),
                         working_copy ? " (working copy)" : "");
            handle_command(srv,
                "{\"type\":\"cmd\",\"id\":1,\"name\":\"open_project\",\"args\":{\"path\":"
                + xp::json_escape(project)
                + (working_copy ? ",\"working_copy\":true" : "") + "}}");

            // Resolve the script path: an explicit --script relative path is
            // relative to the PROJECT dir; otherwise project.json's script_path.
            // open_project ALWAYS populates script_path (falls back to
            // "<folder>/inspection.cpp"), so a script-less project resolves to a
            // path that may not exist — treat a missing script FILE as open-only
            // rather than firing a doomed compile.
            if (!script.empty()) {
                std::filesystem::path sp(script);
                if (sp.is_relative()) sp = std::filesystem::path(project) / sp;
                script = sp.string();
            } else {
                script = g_plugin_mgr.project().script_path;
            }
            if (script.empty() || !std::filesystem::exists(script)) {
                std::fprintf(stderr,
                    "[xinsp2] autostart: no script file (%s); open only\n",
                    script.empty() ? "none given" : script.c_str());
            } else {
                std::fprintf(stderr, "[xinsp2] autostart: compile_and_load %s\n", script.c_str());
                // Production/headless boot wants the OPTIMIZED (/O2) build, not the
                // interactive /Od fast-dev default.
                handle_command(srv,
                    "{\"type\":\"cmd\",\"id\":2,\"name\":\"compile_and_load\",\"args\":{\"path\":"
                    + xp::json_escape(script) + ",\"optimize\":true}}");

                // Confirm the script actually loaded. A failed compile leaves the
                // port up (operator can attach + fix) but the line CANNOT inspect,
                // so mark degraded and withhold "ready" — the FE then treats a
                // non-inspecting line as unhealthy instead of silently "healthy".
                if (!g_script.ok()) {
                    degraded = true;
                    std::fprintf(stderr,
                        "[xinsp2] autostart: degraded - script failed to compile/load; "
                        "line will NOT inspect (port stays up for an operator to recompile)\n");
                } else if (autostart_fps != 0) {
                    // >0 = timer at N fps; <0 = trigger-only (no timer). The fps
                    // value passes through to cmd:start, which treats <=0 as
                    // trigger-only.
                    std::fprintf(stderr, "[xinsp2] autostart: start (fps=%d%s)\n",
                                 autostart_fps, autostart_fps < 0 ? ", trigger-only" : "");
                    handle_command(srv,
                        "{\"type\":\"cmd\",\"id\":3,\"name\":\"start\",\"args\":{\"fps\":"
                        + std::to_string(autostart_fps) + "}}");
                }
            }
        }
        // Debug hook (test-only): simulate a backend that hangs DURING boot —
        // alive, port bound, but never reaches "ready". Used by the FE
        // boot-timeout test (examples/qa_race/driver_boot_hang.py). Placed
        // before the readiness marker so the FE's boot gate trips.
        if (has_flag(argc, argv, "--hang-before-ready")) {
            std::fprintf(stderr, "[xinsp2] autostart: --hang-before-ready (debug) — "
                                 "hanging before ready\n");
            std::fflush(stderr);
            while (!g_should_exit.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            return 0;
        }

        // Readiness marker. The WS port binds in srv.start() ABOVE, but the
        // open/compile/start sequence runs synchronously here before the accept
        // loop — so for several seconds the port is "up" (TCP connect succeeds)
        // while the backend isn't yet serving. A supervisor / operator HMI / a
        // test can wait for THIS line to know the line is actually ready, not
        // merely listening. (port-up != ready.)
        // Only signal "ready" when the line can actually inspect. A degraded
        // (compile-failed) boot deliberately withholds it — the FE then sees no
        // health signal and drives the line safe rather than trusting port-up.
        if (!degraded) std::fprintf(stderr, "[xinsp2] autostart: ready\n");
        std::fflush(stderr);
    }

    // Liveness heartbeat: a monotonic counter written from the SERVING loop.
    // If a synchronous WS handler wedges srv.poll(), the counter stops advancing
    // while the port still accepts TCP — a "bound but not serving" state a
    // connect probe can't see. The FE watches this file (--heartbeat-file) and
    // respawns on staleness. No-op when the flag is unset.
    std::string hb_path = parse_str_flag(argc, argv, "--heartbeat-file");
    uint64_t hb_counter = 0;
    auto write_heartbeat = [&] {
        if (hb_path.empty()) return;
        if (FILE* f = std::fopen(hb_path.c_str(), "wb")) {
            std::fprintf(f, "%llu", (unsigned long long)++hb_counter);
            std::fclose(f);
        }
    };
    write_heartbeat();   // initial beat so the FE sees liveness promptly

    // Debug hook (test-only): wedge the serving loop AFTER ready — port stays
    // bound + accepting but the heartbeat goes stale, so the FE detects a
    // serve-time wedge. Drives examples/qa_race/driver_serve_wedge.py.
    if (has_flag(argc, argv, "--hang-after-ready")) {
        std::fprintf(stderr, "[xinsp2] --hang-after-ready (debug) — wedging the serving loop\n");
        std::fflush(stderr);
        while (!g_should_exit.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return 0;
    }

    int64_t hb_last_ms = 0;
    while (!g_should_exit.load() && srv.is_running()) {
        srv.poll(100);
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - hb_last_ms >= 1000) { write_heartbeat(); hb_last_ms = now; }
    }

    srv.stop();
    g_watchdog_run = false;
    if (g_watchdog_thread.joinable()) g_watchdog_thread.join();
    // Clean shutdown: tell the gateway "bye" so it disarms the dead-man (this is
    // an intended exit, not a crash) before disconnecting.
    if (g_gateway) { g_gateway->say_bye(); g_gateway->stop(); g_gateway = nullptr; }
    std::fprintf(stderr, "[xinsp2] shutdown complete\n");
    return 0;
}
