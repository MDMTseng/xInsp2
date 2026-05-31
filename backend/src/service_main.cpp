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
// Worker thread pool. project.json `parallelism.dispatch_threads: N`
// controls the size; default 1 (current behaviour). All workers pull
// from the same g_ev_queue. A separate timer thread (`g_timer_thread`)
// pushes a synthetic empty event at the configured fps so scripts
// that don't have a trigger source still get periodic dispatch.
static std::vector<std::thread> g_worker_threads;
static std::thread              g_timer_thread;
// Result ordering (parallelism.result_order). When g_result_ordered is set at
// cmd:start (== "arrival" && N>1), each popped event gets a gapless emit
// sequence (g_dispatch_seq, assigned under g_ev_mu so it follows arrival order)
// and an EmitTurn gate makes workers emit vars/previews/run_finished in that
// order. Compute still runs fully parallel; only emission is serialized. In
// "completion" mode g_result_ordered is false and emit_seq is -1 (emit
// immediately, as before).
static std::atomic<bool>       g_result_ordered{false};
static std::atomic<int64_t>    g_dispatch_seq{0};   // next emit seq (per cmd:start)
static std::mutex              g_emit_order_mu;
static std::condition_variable g_emit_order_cv;
static int64_t                 g_emit_next = 0;      // guarded by g_emit_order_mu

// RAII emit-order gate. For emit_seq >= 0 (ordered mode) the ctor blocks until
// it's this sequence's turn; the dtor advances the cursor + wakes the next
// worker — even on an exception or an error path, so a crashed inspect can't
// stall the stream. emit_seq < 0 (completion mode / cmd:run) is a no-op.
struct EmitTurn {
    int64_t seq_;
    bool    on_;
    explicit EmitTurn(int64_t seq) : seq_(seq), on_(seq >= 0) {
        if (!on_) return;
        std::unique_lock<std::mutex> lk(g_emit_order_mu);
        g_emit_order_cv.wait(lk, [this] {
            return g_emit_next == seq_ || !g_continuous.load();
        });
    }
    ~EmitTurn() {
        if (!on_) return;
        {
            std::lock_guard<std::mutex> lk(g_emit_order_mu);
            if (g_emit_next == seq_) ++g_emit_next;   // skip if we ran early on stop
        }
        g_emit_order_cv.notify_all();
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
                               int64_t emit_seq = -1);

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
// open+compile and wait for a client / triggers).
static int parse_autostart_fps(int argc, char** argv) {
    std::string v = parse_str_flag(argc, argv, "--autostart-fps");
    if (v.empty()) return 0;
    try { int n = std::stoi(v); return n < 0 ? 0 : n; } catch (...) { return 0; }
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
                               int64_t emit_seq) {
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
        EmitTurn turn(emit_seq);
        if (inspect_ok) {
            emit_vars_and_previews(srv, s, run_id, dt_ms);
            emit_run_event("run_finished",
                           "\"ms\":" + std::to_string((long long)dt_ms));
            // Clear so the next run, if it doesn't carry a frame_path arg,
            // sees an empty path instead of the stale previous one.
            if (s.set_run_context) s.set_run_context("");
        } else {
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
        for (auto& [src, h] : ev.images) xi::ImagePool::instance().release(h);
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
    for (auto& [src, h] : front.images) xi::ImagePool::instance().release(h);
    g_ev_queue.pop_front();
    g_ev_queue.push_back(std::move(ev));
    g_ev_cv.notify_one();
    ++g_dropped_oldest;
    return true;
}

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
    g_worker_threads.clear();
    g_worker_threads.reserve((size_t)n_threads);
    // Result ordering: arrival-ordered emission only when explicitly asked AND
    // there's actual concurrency (N>1 — N==1 is already in order). Reset the
    // sequence cursors so each (re)start counts from 0. Covers both cmd:start
    // and the hot-reload re-arm, since both come through here.
    bool ordered = (g_plugin_mgr.project().result_order == "arrival") && n_threads > 1;
    g_result_ordered.store(ordered);
    g_dispatch_seq.store(0);
    { std::lock_guard<std::mutex> lk(g_emit_order_mu); g_emit_next = 0; }
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

    // Timer thread: every interval_ms push a synthetic empty event so
    // scripts without trigger sources still tick. Goes through the
    // queue-policy helper so synthetic events also get dropped under
    // backpressure rather than infinitely accumulating.
    g_timer_thread = std::thread([interval_ms] {
        while (g_continuous.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            if (!g_continuous.load()) break;
            (void)enqueue_event_(xi::TriggerEvent{});
        }
    });
}

// Stop the pool + timer. Safe to call if nothing was spawned.
static void stop_dispatch_pool_() {
    g_continuous = false;
    g_ev_cv.notify_all();
    g_emit_order_cv.notify_all();   // wake any worker parked in an EmitTurn
    if (g_timer_thread.joinable()) g_timer_thread.join();
    for (auto& t : g_worker_threads) {
        if (t.joinable()) t.join();
    }
    g_worker_threads.clear();
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
        if (was_continuous) {
            int fps = prior_continuous_fps > 0 ? prior_continuous_fps : 10;
            g_continuous_fps = fps;
            g_continuous = true;
            int interval_ms = 1000 / std::max(fps, 1);
            int n_threads = g_plugin_mgr.project().dispatch_threads;
            xi::TriggerBus::instance().set_sink([](xi::TriggerEvent ev) {
                (void)enqueue_event_(std::move(ev));
            });
            spawn_dispatch_pool_(&srv, interval_ms, n_threads);
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
        crash_set(crash_ctx().last_cmd, sizeof(crash_ctx().last_cmd), "run");
        crash_ctx().last_run_id = (int)run_id;
        std::thread([&srv, run_id, frame_path = std::move(frame_path)]() {
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

        // Parse optional fps from args (default 10)
        int fps = 10;
        auto fps_val = xp::get_number_field(parsed->args_json, "fps");
        if (fps_val && *fps_val > 0) fps = (int)*fps_val;

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

        g_continuous_fps = fps;
        g_continuous = true;
        // Reset queue stats so each cmd:start gets a fresh observation
        // window. Keeps `dispatch_stats` per-run comparable.
        g_dropped_oldest = 0;
        g_dropped_newest = 0;
        g_queue_high_watermark = 0;

        int interval_ms = 1000 / std::max(fps, 1);
        int n_threads = g_plugin_mgr.project().dispatch_threads;
        if (n_threads < 1) n_threads = 1;

        // Bus-driven dispatch: events arrive via TriggerBus sink → enqueued
        // → workers pop and run inspect with that trigger as current.
        // Timer thread emits synthetic events on schedule for scripts
        // without trigger sources.
        xi::TriggerBus::instance().set_sink([](xi::TriggerEvent ev) {
            (void)enqueue_event_(std::move(ev));
        });

        spawn_dispatch_pool_(&srv, interval_ms, n_threads);

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
        (void)quiesce_dispatch_for_lifecycle_op_("open_project");
        if (g_plugin_mgr.open_project(*folder)) {
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
        // Ensure plugin is loaded
        g_plugin_mgr.load_plugin(*plugin);
        auto* ii = g_plugin_mgr.create_instance(*iname, *plugin);
        if (ii) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "failed to create instance");
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
        default:                                return "UNKNOWN";
    }
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
                "  --comms-port=N       connect to the comms gateway on loopback N (xi::comms)\n"
                "  --version, -v        print version and exit\n"
                "  --help, -h           this help\n",
                XINSP2_VERSION);
            return 0;
        }
    }

    int port = parse_port(argc, argv);

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
            std::fprintf(stderr, "[xinsp2] autostart: open_project %s\n", project.c_str());
            handle_command(srv,
                "{\"type\":\"cmd\",\"id\":1,\"name\":\"open_project\",\"args\":{\"path\":"
                + xp::json_escape(project) + "}}");

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
                handle_command(srv,
                    "{\"type\":\"cmd\",\"id\":2,\"name\":\"compile_and_load\",\"args\":{\"path\":"
                    + xp::json_escape(script) + "}}");

                // Confirm the script actually loaded. A failed compile leaves the
                // port up (operator can attach + fix) but the line CANNOT inspect,
                // so mark degraded and withhold "ready" — the FE then treats a
                // non-inspecting line as unhealthy instead of silently "healthy".
                if (!g_script.ok()) {
                    degraded = true;
                    std::fprintf(stderr,
                        "[xinsp2] autostart: degraded - script failed to compile/load; "
                        "line will NOT inspect (port stays up for an operator to recompile)\n");
                } else if (autostart_fps > 0) {
                    std::fprintf(stderr, "[xinsp2] autostart: start %d fps\n", autostart_fps);
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
