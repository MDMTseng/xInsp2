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
#include <random>
#include <typeinfo>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <yyjson.h>
// (The SDK umbrella <xi/xi.hpp> is intentionally NOT included — it pulls the
//  script-side SDK headers (xi_param/xi_io/xi_state/xi_result/xi_async) the
//  backend binary never needs. Only the core headers it actually uses are
//  included below.)
#include <xi/xi_image.hpp>
#include <xi/xi_cli_args.hpp>
// [v12 THE CUT — <xi/xi_jpeg.hpp> (in-core encoder) include removed; encode is
//  now capability-only via xi.jpeg.encode.]
#include <xi/xi_jpeg_cap.hpp>    // polaris2 ENCODE eviction: compress_sink delegates to xi.jpeg.encode
#include <xi/xi_protocol.hpp>
#include <xi/xi_plugin_manager.hpp>
#include <xi/xi_certify.hpp>      // Part III G1: --certify-plugin child mode (scan/certification isolation)
#include <xi/xi_cap_abi.hpp>      // capability plane pilot (doc 14): service fault-note/culprit hooks
#include <xi/xi_project.hpp>
#include <xi/xi_owner_lock.hpp>     // F5: advisory single-writer stamp on the project folder
#include <cassert>
#include <xi/xi_trigger_bus.hpp>
#include <xi/xi_inflight_runs.hpp>   // xi::InflightRuns (detached-run lifetime owner)
#include <xi/xi_emit_gate.hpp>       // xi::EmitGate / xi::EmitTurn (ordered-emit gate)
#include <xi/xi_metrics.hpp>         // OQ-7a: frame counters + latency histogram (cmd:metrics)
#include <xi/xi_script_compiler.hpp>
#include <xi/xi_toolchain.hpp>       // lens 2#6: VS toolchain / override subsystem (extracted)
#include <xi/xi_script_loader.hpp>
#include <xi/xi_ws_server.hpp>

#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <thread>

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

#include "service_internal.hpp"

// The Engine struct + all shared globals/structs/constants/helpers moved to the
// PRIVATE header service_internal.hpp (behavior-preserving split). The single
// DEFINITION of g_eng lives HERE (this TU); every other service_*.cpp sees it via
// `extern Engine g_eng;` in the header.
Engine g_eng;


// Loaded user script state. When null, cmd:run returns an error.

// [v12 THE CUT — the Record persistent-state channel (persistent_state_json +
//  xi_script_state_schema_version) was DELETED. Cross-frame script state is now
//  the kv channel (persistent_kv_bytes + XI_KV_SCHEMA / migrate_kv), doc 16.]

// Cache of every successful `cmd:set_param` value the backend pushed
// into the live script. compile_and_load replays these into the new
// DLL via xi_script_set_param so user-tuned slider values aren't
// silently reset to file-scope defaults across a recompile. Keyed by
// param name → JSON-encoded scalar (number / bool / string token,
// same shape as set_param's `value` arg). Protected by g_eng.script_mu.

// Cache of every successful script-side `cmd:set_instance_def` def the
// backend pushed into the live script. Exact sibling of g_eng.param_cache:
// script-declared xi::Instance objects live in the script DLL's OWN
// registry and their file-scope ctors re-seed the SOURCE default def on
// every reload, so without replaying the operator-tuned/taught/calibrated
// def the hot-recompile loop silently reverts each instance to its source
// default. Keyed by instance name → def JSON (same shape as set_instance_def's
// `def` arg). Only the SCRIPT-instance path is cached — backend plugin-manager
// instances persist via InstanceRegistry across a script recompile. Protected
// by g_eng.script_mu like g_eng.param_cache.

// --- xi::use() callback implementations ---
// These are called FROM the script DLL back INTO the backend, routing
// process/exchange/grab to the backend's InstanceRegistry.

#include <xi/xi_use.hpp>
#include <xi/xi_seh.hpp>
#include <xi/xi_graph_capture.hpp>
#include <xi/xi_crash_dump.hpp>   // xi::crash:: minidump + breadcrumb forensics (extracted leaf)

using xi::seh_exception;
// (seh translation installed via xi::install_seh_translator() — portable shim)

// ---- host_api callback surface (graph/sinks/trigger/owner/watchdog-slot)
//      extracted to service_sinks.cpp -----------------------------------------

// ---- status registry + per-run result extracted to service_result.cpp -------
// ---- Comms ------------------------------------------------------------------
// The out-of-process comms gateway + the xi::comms script API were removed: PLC
// I/O is now a plugin concern (a comm plugin owns the socket), and the BE-crash
// "go safe" case is the plugin's own sidecar process (it watches the BE and
// sends the line-safe message on death). See docs/archive/comms-gateway.md.

// Forward-declare: runs one inspection cycle (drives sinks + emits the run result).
// If run_id == 0, auto-generates one. frame_hint is passed to inspect().
// frame_path (optional) is plumbed to the script via
// `xi_script_set_run_context`; readable inside the script as
// `xi::current_frame_path()`. Empty string means none.
// run_one_inspection declared (with default args) in service_internal.hpp.

// Path resolution for the script compiler. Backend derives its own dir at
// startup and uses that to locate the xi headers we ship alongside the exe.
// Accelerator install roots, probed once at startup. Empty string =
// not installed → user scripts fall back to portable C++ for that path.
// vcvars64.bat override (empty = let the compiler auto-find via auto_find_vcvars).
// Canonical folder of the currently-open project (the one the user edits — NOT
// any .xinsp_work scratch). Set on open_project; used to read/write the
// per-project "toolchain" override block in its project.json.
// The xi include dir derived from the exe location at startup. Kept separate from
// g_eng.include_dir so a project override can point elsewhere yet we can always fall
// back to the shipped headers.

// IntelliSense config: the C/C++ extension (Microsoft) has no way to know our
// compile flags — inspect.cpp / plugin .cpp are compiled by the backend, not by
// CMake — so #include <xi/xi.hpp> and <opencv2/...> light up red. The fix (a
// c_cpp_properties.json mirroring the compiler's include set) is now written by
// the VS Code EXTENSION, which reads the resolved paths via cmd:toolchain_health.
// The core no longer touches .vscode — it only EXPOSES the paths (below).

// ---- toolchain resolution / script-deps / DLL-search / crash-breadcrumbs
//      extracted to service_toolchain.cpp ------------------------------------

// T2: set at the end of controlled_shutdown_teardown_ so the console-control
// handler can wait for a clean teardown before the OS force-terminates on a
// window close / logoff / shutdown (which give only a short grace window).

// CLI/env arg parsing (get_exe_dir / parse_port / parse_host / parse_watchdog_ms
// / parse_auth_secret / parse_str_flag / has_flag / parse_autostart_fps /
// parse_extra_plugin_dirs) moved to xi/xi_cli_args.hpp (namespace xi::cli).

double now_seconds() { return xi::wall_us() / 1e6; }   // wall: pong ts

void send_rsp_ok(xi::ws::Server& srv, int64_t id, std::string data_json) {
    xp::Rsp r;
    r.id = id;
    r.ok = true;
    r.data_json = std::move(data_json);
    srv.send_text(r.to_json());
}

// Ring buffer of recent error messages so an AI / scripted client can
// correlate a synchronous cmd with any side-channel errors that might
// have raced in (run-thread crashes, log-level=error from background
// activity, etc). Three error channels exist
// in the protocol — rsp.error (sync), `event` (async), `log`
// level=error (async) — and the WS spec doesn't carry cmd_id /
// run_id on the async two. Until that's fixed protocol-wide, this
// ring lets the client pull "anything error-shaped that happened
// in the last minute" with a single query.
// kRecentErrorsCap moved to service_internal.hpp.
int64_t now_ms_() { return xi::wall_ms(); }   // wall: RecentError ts

void push_recent_error(std::string source, std::string message,
                              int64_t cmd_id, int64_t run_id) {
    RecentError e{ now_ms_(), std::move(source), std::move(message), cmd_id, run_id };
    std::lock_guard<std::mutex> lk(g_eng.recent_errors_mu);
    g_eng.recent_errors.push_back(std::move(e));
    while (g_eng.recent_errors.size() > kRecentErrorsCap) g_eng.recent_errors.pop_front();
}

void send_rsp_err(xi::ws::Server& srv, int64_t id, std::string err) {
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
void emit_error_log(xi::ws::Server& srv, const std::string& msg,
                           int64_t run_id) {
    xp::LogMsg lm; lm.level = "error"; lm.msg = msg;
    srv.send_text(lm.to_json());
    push_recent_error("log", msg, /*cmd_id=*/0, run_id);
}

void send_hello(xi::ws::Server& srv) {
    xp::Event e;
    e.name = "hello";
    e.data_json = std::string(R"({"version":")") + XINSP2_VERSION
                + R"(","commit":")" + XINSP2_COMMIT
                + R"(","abi":2})";
    srv.send_text(e.to_json());
}


// (seh_exception and seh_translator defined above, before use_process_cb)

// ---- single-frame inspection cycle extracted to service_inspect.cpp ---------

// (trigger_worker removed — continuous mode uses a simple timer thread)

// ---- Dispatch groups / pool lifecycle / trigger sink / teardown / instance-state
//      extracted to service_dispatch.cpp ------------------------------------

// ============================================================================
// WS command handlers  (TASTE refactor, lens 2#2): the former handle_command
// if/else-if god-chain, split into one named handler per command + a dispatch
// table (g_cmd_table).  Each handler body is byte-identical to the arm it came
// from; only the control-flow wrapper changed.  Grouped by capability below.
// Uniform signature: (xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed)
// -- bodies reference srv / id / parsed->args_json exactly as the arms did.
// ============================================================================

// ---- lifecycle handlers extracted to service_cmd_lifecycle.cpp -------------
// ---- dispatch-control handlers extracted to service_cmd_dispatch.cpp --------
// ---- observability handlers extracted to service_cmd_observability.cpp ------
// ---- project-CRUD handlers extracted to service_cmd_project.cpp ------------
// ---- plugin-mgmt handlers extracted to service_cmd_plugin.cpp --------------

// Dispatch table: command name -> handler.  Replaces the if/else-if chain.
using HandlerFn = void(*)(xi::ws::Server&, int64_t, const xp::ParsedCmd*);
static const std::unordered_map<std::string_view, HandlerFn> g_cmd_table = {
    {"ping", cmd_ping_},
    {"version", cmd_version_},
    {"crash_reports", cmd_crash_reports_},
    {"clear_crash_reports", cmd_clear_crash_reports_},
    {"set_watchdog_ms", cmd_set_watchdog_ms_},
    {"set_process_priority", cmd_set_process_priority_},
    {"set_timer_fps", cmd_set_timer_fps_},
    {"graph_capture", cmd_graph_capture_},
    {"graph_snapshot", cmd_graph_snapshot_},
    {"shutdown", cmd_shutdown_},
    {"compile_and_load", cmd_compile_and_load_},
    {"run", cmd_run_},
    {"start", cmd_start_},
    {"stop", cmd_stop_},
    {"list_params", cmd_list_params_},
    {"set_param", cmd_set_param_},
    {"list_instances", cmd_list_instances_},
    {"set_instance_def", cmd_set_instance_def_},
    {"get_instance_def", cmd_get_instance_def_},
    {"exchange_instance", cmd_exchange_instance_},
    {"get_state", cmd_get_state_},
    {"prepare_instance", cmd_prepare_instance_},
    {"commit_group", cmd_commit_group_},
    {"save_project", cmd_save_project_},
    {"commit_working_copy", cmd_commit_working_copy_},
    {"discard_working_copy", cmd_discard_working_copy_},
    {"load_project", cmd_load_project_},
    {"list_plugins", cmd_list_plugins_},
    {"recent_errors", cmd_recent_errors_},
    {"status", cmd_status_},
    {"image_pool_stats", cmd_image_pool_stats_},
    {"rescan_plugins", cmd_rescan_plugins_},
    {"create_project", cmd_create_project_},
    {"open_project", cmd_open_project_},
    {"close_project", cmd_close_project_},
    {"export_project_plugin", cmd_export_project_plugin_},
    {"recompile_project_plugin", cmd_recompile_project_plugin_},
    {"rebuild_plugins", cmd_rebuild_plugins_},
    {"dispatch_stats", cmd_dispatch_stats_},
    {"metrics", cmd_metrics_},
    {"open_project_warnings", cmd_open_project_warnings_},
    {"create_instance", cmd_create_instance_},
    {"remove_instance", cmd_remove_instance_},
    {"rename_instance", cmd_rename_instance_},
    {"save_instance_config", cmd_save_instance_config_},
    {"get_plugin_ui", cmd_get_plugin_ui_},
    {"get_dashboard", cmd_get_dashboard_},
    {"toolchain_health", cmd_toolchain_health_},
    {"set_toolchain_override", cmd_set_toolchain_override_},
    {"get_health", cmd_get_health_},
};

// One dispatch shell (adoption-map item 1 / review 09 findings 1-2). The parse,
// malformed-envelope handling, handler lookup, and the top-level exception guard
// all live in xp::dispatch_command_guarded; this is its only production wiring.
// A handler that throws now becomes `rsp ok:false` correlated to the command id
// (the documented contract, ws-protocol.md "Error handling") instead of escaping
// the serve loop into std::terminate. A malformed envelope with a recoverable id
// gets a correlated error rather than stalling the client to its timeout, and
// every malformed reject bumps the counter surfaced by dispatch_stats.
static void handle_command(xi::ws::Server& srv, std::string_view text) {
    xp::dispatch_command_guarded(
        text,
        /*send_err=*/[&](int64_t id, std::string msg) { send_rsp_err(srv, id, std::move(msg)); },
        /*send_log=*/[&](std::string msg) {
            xp::LogMsg lm; lm.level = "error"; lm.msg = std::move(msg);
            srv.send_text(lm.to_json());
        },
        /*on_reject=*/[] { g_eng.malformed_cmd_rejected.fetch_add(1, std::memory_order_relaxed); },
        /*invoke=*/[&](std::string_view name, int64_t id, const xp::ParsedCmd* parsed) -> bool {
            auto it = g_cmd_table.find(name);
            if (it == g_cmd_table.end()) return false;
            it->second(srv, id, parsed);
            return true;
        });
}


// Generate the per-process boot_id (random 128-bit → 32-char lowercase hex) and
// read the optional station_id from XINSP_STATION_ID. Runs ONCE at startup (not
// per-frame), so std::random_device + a seeded 64-bit engine is fine here. Both
// land in g_eng and are read-only afterward, so every run_result emission can
// read them lock-free.
static void init_process_identity_() {
    std::random_device rd;
    std::seed_seq seed{ rd(), rd(), rd(), rd(),
                        (unsigned)std::chrono::steady_clock::now().time_since_epoch().count() };
    std::mt19937_64 eng(seed);
    uint64_t hi = eng(), lo = eng();
    if (hi == 0 && lo == 0) lo = 1;   // never all-zero (would read as "null")
    g_eng.boot_id = trigger_id_hex(xi_trigger_id{ hi, lo });

    if (const char* s = std::getenv("XINSP_STATION_ID"); s && *s)
        g_eng.station_id = s;
}

int main(int argc, char** argv) {
    // Part III G1.1 — certify mode: load a plugin DLL + call its factory once in
    // THIS throwaway child process, exit with a verdict code (0 ok / 42
    // abi_mismatch / abnormal = crashed). Crash-isolation for discovery: a
    // malformed DLL faults HERE, never in the scanning backend. Handled BEFORE
    // install_seh_translator() so a fault reaches the minidump filter rather than
    // being translated to a catchable exception + swallowed.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--certify-plugin") {
            const char* dir = (i + 1 < argc) ? argv[i + 1] : nullptr;
            xi::crash::install();   // a crashed certify still yields a minidump
            int code = dir ? xi::certify::certify_in_process(dir)
                           : xi::certify::kExitAbiMismatch;
            std::fflush(stderr);
            std::fflush(stdout);
            return code;
        }
    }

    // Install the crash-forensics handlers (minidump filter + CRT death-path
    // interceptors + first-chance logger + fault-stack reserve). Lives in the
    // extracted leaf xi_crash_dump.hpp.
    xi::crash::install();
    // SEH → C++ exception translator so try/catch catches segfaults (a separate
    // concern from the dump machinery; owned here, re-set per inspect thread).
    xi::install_seh_translator();

    // --test-crash: deliberately trigger a fatal exception so the
    // top-level minidump filter fires. Used by runCrashDump E2E.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--test-crash") {
#ifdef _WIN32
            RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
            // POSIX: a genuine access violation so the SIGSEGV crash handler fires,
            // mirroring the Win32 unhandled-exception filter path.
            std::raise(SIGSEGV);
#endif
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
                "  --priority=CLASS     process priority: high|above|normal|below|realtime (Win)\n"
                "  --aot                prebuilt bundle: load existing plugin/script DLLs, no compiler\n"
                "  --version, -v        print version and exit\n"
                "  --help, -h           this help\n",
                XINSP2_VERSION);
            return 0;
        }
    }

    int port = xi::cli::parse_port(argc, argv);

    // Per-process identity for run_result (boot_id + optional station_id). Once,
    // early, before any inspection can emit. See init_process_identity_.
    init_process_identity_();

    // ---- thread/process performance knobs --------------------------------------
#ifdef _WIN32
    // Raise the OS timer resolution to 1ms (default ~15.6ms) so timer-tick fps,
    // sleeps, and CV waits are tight. winmm.timeBeginPeriod via runtime-load so we
    // don't add a link dependency. Process-wide; the paired timeEndPeriod is optional.
    if (HMODULE w = LoadLibraryA("winmm.dll")) {
        if (auto fn = (UINT(WINAPI*)(UINT))GetProcAddress(w, "timeBeginPeriod")) fn(1);
    }
    // --priority=<class>: bump the whole backend's process priority (for a
    // dedicated inspection PC). Default = leave as-is. Also settable live via
    // cmd:set_process_priority / project.json runtime.process_priority.
    if (std::string pri = xi::cli::parse_str_flag(argc, argv, "--priority"); !pri.empty()) {
        if (!apply_process_priority_(pri))
            std::fprintf(stderr,
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
        std::filesystem::path p = xi::cli::get_exe_dir();
        for (int i = 0; i < 6; ++i) {
            if (std::filesystem::exists(p / "include" / "xi" / "xi.hpp")) {
                g_eng.include_dir = (p / "include").string();
                break;
            }
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
        if (g_eng.include_dir.empty()) {
            // Fallback: next to the exe.
            g_eng.include_dir = (std::filesystem::path(xi::cli::get_exe_dir()) / "include").string();
        }
        // Remember the shipped headers as the default a project override falls
        // back to (see resolve_toolchain_).
        g_eng.include_dir_default = g_eng.include_dir;
    }
    // J1 (RT5): per-PID so co-resident backends never share script_build + .pch.
    // Two backends CAN coexist (SO_EXCLUSIVEADDRUSE blocks only the same port;
    // --port is configurable and the QA free_port() harness runs many at once), and
    // the version counter resets to 0 each start → both would target inspect_v0.dll
    // and the fixed-name umbrella.{pch,obj} in one shared dir: one's remove(out_dll)
    // deletes the other's fresh DLL, or a /Yc PCH write races a /Yu read → corrupt
    // load / spurious C1852. A per-PID subdir removes the sharing entirely.
    // Per-PID dirs would otherwise accumulate (~200 MB PCH each) and fill the disk;
    // reap the ones left by dead backends before claiming ours.
    std::filesystem::path _xi_base = std::filesystem::temp_directory_path() / "xinsp2";
    xi::script::reap_stale_build_dirs(_xi_base);
    g_eng.work_dir = (_xi_base / std::to_string(GetCurrentProcessId())).string();
    // A reused PID may leave a stale dir from a dead process — start clean.
    std::error_code _wd_ec;
    std::filesystem::remove_all(g_eng.work_dir, _wd_ec);
    std::filesystem::create_directories(g_eng.work_dir);

    // Probe accelerators once. Logged so the user can see what their
    // compiled scripts will inherit.
    g_eng.opencv_dir     = xi::script::detail::probe_opencv_dir();
    g_eng.turbojpeg_root = xi::script::detail::probe_turbojpeg_root();
    g_eng.ipp_root       = xi::script::detail::probe_ipp_root();
    std::fprintf(stderr, "[xinsp2] script-side accelerators: opencv=%s  turbojpeg=%s  ipp=%s\n",
                 g_eng.opencv_dir.empty()     ? "no" : g_eng.opencv_dir.c_str(),
                 g_eng.turbojpeg_root.empty() ? "no" : g_eng.turbojpeg_root.c_str(),
                 g_eng.ipp_root.empty()       ? "no" : g_eng.ipp_root.c_str());

    // Find and scan plugins directory (sibling of backend/)
    {
        std::filesystem::path p = xi::cli::get_exe_dir();
        for (int i = 0; i < 6; ++i) {
            if (std::filesystem::exists(p / "plugins")) {
                g_eng.plugins_dir = (p / "plugins").string();
                break;
            }
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
    }
    // G1.3 — certify each discovered plugin in a throwaway child (this same
    // backend exe, --certify-plugin mode) before arming it during the scan. A DLL
    // that crashes certification is skipped + surfaced (g_eng.plugin_mgr.certify_
    // warnings()), so discovery can never load a known-bad DLL into the backend.
    {
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (n) g_eng.plugin_mgr.set_certify_exe(std::string(exe, n));
    }
    if (!g_eng.plugins_dir.empty()) {
        int n = g_eng.plugin_mgr.scan_plugins(g_eng.plugins_dir);
        std::fprintf(stderr, "[xinsp2] scanned %d plugins from %s\n", n, g_eng.plugins_dir.c_str());
    }
    // Additional plugin folders from --plugins-dir / XINSP2_EXTRA_PLUGIN_DIRS.
    // Lets external SDKs keep their plugin DLLs in place — no copy needed.
    for (auto& dir : xi::cli::parse_extra_plugin_dirs(argc, argv)) {
        if (!std::filesystem::exists(dir)) {
            std::fprintf(stderr, "[xinsp2] extra plugin dir not found: %s\n", dir.c_str());
            continue;
        }
        int n = g_eng.plugin_mgr.scan_plugins(dir);
        std::fprintf(stderr, "[xinsp2] scanned %d plugins from %s\n", n, dir.c_str());
    }

    // G1.3 / G2.2 — surface any plugin gated out at discovery (certify crashed, or
    // FE-quarantined) into the recent-errors ring so cmd:recent_errors + the
    // extension toast tell the operator WHICH plugin is disabled and why (not just
    // a silently-missing plugin). The scan already logged each to stderr.
    for (auto& w : g_eng.plugin_mgr.certify_warnings())
        push_recent_error("plugin", w.reason);

    std::fprintf(stderr, "[xinsp2] include_dir=%s\n", g_eng.include_dir.c_str());
    std::fprintf(stderr, "[xinsp2] work_dir=%s\n",    g_eng.work_dir.c_str());
    std::fprintf(stderr, "[xinsp2] plugins_dir=%s\n",  g_eng.plugins_dir.c_str());

    // V3 machine-level lib-plugin autoload (docs/new_gen/14, doc 19 V3): bring up
    // every discovered plugin marked `"autoload": true` (a `lib` capability
    // provider, e.g. imgcodec) ONCE now, under a stable machine owner, so its
    // capabilities (xi.image.decode / xi.jpeg.encode) are available WITHOUT any
    // project declaring a per-instance — clearing E1's second cause (doc 06 §6).
    // Machine-scoped: these persist across project open/close; an explicit
    // project instance of the same plugin still takes precedence (it displaces
    // the machine provider for the life of the project). DEPLOYMENT OPT-IN:
    // gated on `--autoload-lib` (or env XINSP2_AUTOLOAD_LIB) so a stock
    // deployment is byte-unchanged — nothing that keys off capability
    // availability (e.g. expose's E2 JPEG preview, which activates when
    // xi.jpeg.encode is present) flips implicitly. The app team enables this once
    // per deployment (E1). Runs after the plugin scan; the first factory call
    // publishes the cap plane via default_host_api().
    {
        const char* env = std::getenv("XINSP2_AUTOLOAD_LIB");
        bool autoload_lib = xi::cli::has_flag(argc, argv, "--autoload-lib") ||
                            (env && *env && std::strcmp(env, "0") != 0);
        g_eng.plugin_mgr.set_autoload_enabled(autoload_lib);
        if (autoload_lib) {
            int nlib = g_eng.plugin_mgr.autoload_machine_providers();
            std::fprintf(stderr, "[xinsp2] lib autoload ENABLED — %d machine "
                                 "provider(s) up\n", nlib);
        }
    }

    // Process isolation + SHM removed 2026-05: all plugins run
    // in-process and share the host ImagePool directly (zero-copy via
    // pointers, no cross-process marshalling). No worker process, no
    // shared-memory region to set up.

    // Hand the same compile environment that xi::script::compile uses
    // to the plugin manager — project plugins (compiled when a project
    // is opened) need the include dir, vcvars, and accelerator roots.
    xi::CompileEnv env;
    env.include_dir    = g_eng.include_dir;
    env.opencv_dir     = g_eng.opencv_dir;
    env.turbojpeg_root = g_eng.turbojpeg_root;
    env.ipp_root       = g_eng.ipp_root;
    // --aot: this is a prebuilt bundle — load existing plugin DLLs instead of
    // compiling (no cl.exe on the target). The autostart script should point at a
    // .dll too (compile_and_load loads a .dll directly).
    env.aot = xi::cli::has_flag(argc, argv, "--aot");
    if (env.aot) std::fprintf(stderr, "[xinsp2] AOT mode: loading prebuilt plugin/script DLLs (no compiler)\n");
    g_eng.plugin_mgr.set_compile_env(env);

    xi::ws::Server srv;
    srv.on_open  = [&] {
        std::fprintf(stderr, "[xinsp2] client connected\n");
        send_hello(srv);
    };
    srv.on_close = [&] {
        std::fprintf(stderr, "[xinsp2] client disconnected\n");
        // E-P1-2: a fresh client should get a fresh server view. Without this
        // clear the next driver to reconnect inherits the prior session's error
        // ring — not visible from the new client's perspective, at best confuses,
        // at worst hides regressions.
        {
            std::lock_guard<std::mutex> lk(g_eng.recent_errors_mu);
            g_eng.recent_errors.clear();
        }
        // (E-P1-1: the per-instance-death dedup set was removed with
        // process isolation in 2026-05; no set to clear here.)
    };
    srv.on_text = [&](std::string_view s) {
        handle_command(srv, s);
    };
    srv.on_binary = [&](const uint8_t*, size_t n) {
        std::fprintf(stderr, "[xinsp2] unexpected binary frame: %zu bytes\n", n);
    };

    std::string host   = xi::cli::parse_host(argc, argv);
    std::string secret = xi::cli::parse_auth_secret(argc, argv);
    srv.set_bind_host(host);
    if (!secret.empty()) srv.set_auth_secret(secret);

    g_eng.watchdog_ms = xi::cli::parse_watchdog_ms(argc, argv);
    if (g_eng.watchdog_ms.load() > 0) {
        std::fprintf(stderr, "[xinsp2] watchdog enabled: %d ms per inspect\n", g_eng.watchdog_ms.load());
    }
    g_eng.srv_for_bp = &srv;   // status_cb + dropped-frame markers emit through it
    // Health/state contract (docs/new_gen/04-health-contract.md): route the
    // registry's health_changed events to WS clients, and — when --health-file is
    // given (the FE passes it) — mirror the top-level state to that file so the FE
    // can surface it in fe_status WITHOUT holding the single WS client slot. State
    // starts at `boot`.
    std::string health_file = xi::cli::parse_str_flag(argc, argv, "--health-file");
    install_health_notifier_(health_file);
    // Route plugin host_api->set_status into the status registry. Non-capturing
    // so it converts to the StatusSinkFn function pointer.
    xi::status_sink() = [](const char* who, const char* text) {
        set_status_internal((who && *who) ? who : "@plugin", text);
    };
    // ABI v8: route plugin host_api->emit_binary straight to WS clients. The core
    // is a dumb byte pipe — the frame format is the plugin's contract with its UI.
    // Non-capturing → converts to the BinarySinkFn function pointer. Thread-safe:
    // send_binary may be called from a dispatch worker (any sink's binary push path).
    xi::binary_sink() = [](const void* data, int len) {
        if (auto* s = g_eng.srv_for_bp.load(std::memory_order_acquire))
            s->send_binary(static_cast<const uint8_t*>(data), static_cast<size_t>(len));
    };
    // ABI v9: a generic JPEG-encode host service — process-global N-rotate cache
    // keyed by a content hash of the pixels, so the SAME image compressed by
    // several plugins (or repeatedly) is encoded ONCE globally. Plugin-agnostic
    // convenience. Non-capturing → converts to CompressSinkFn.
    xi::compress_sink() = [](const void* px, int w, int h, int c, int q,
                             void* out, int cap) -> int {
        if (!px || w <= 0 || h <= 0 || c <= 0) return 0;
        const uint8_t* p = static_cast<const uint8_t*>(px);
        xi::Image img = xi::Image::view(w, h, c, const_cast<uint8_t*>(p));
        std::vector<uint8_t> jpeg;
        // [v12 THE CUT — capability-only: route through the "xi.jpeg.encode"
        // capability (the imgcodec lib plugin, which owns the dedup content
        // cache + the turbojpeg SIMD path). The in-core encoder (xi::encode_jpeg)
        // and the host memo cache were DELETED — no in-core fallback. Absent /
        // funnel refusal / handler fault / contract $fault → 0 (no encode). Per-
        // call availability re-check, reentrancy refusal, and SEH fault
        // attribution to the lib instance live in encode_via_capability.]
        if (!xi::encode_via_capability(img, q, jpeg) || jpeg.empty()) return 0;
        if ((int)jpeg.size() > cap) return -(int)jpeg.size();
        std::memcpy(out, jpeg.data(), jpeg.size());
        return (int)jpeg.size();
    };

    // Capability plane pilot (docs/new_gen/14): enrich the header-side funnel
    // fault mechanics (on_fault policy + health overlay, xi_cap_abi.hpp) with
    // the service's crash bookkeeping — the same note_instance_crash_ (crash-
    // loop count + degraded overlay + recent-errors ring) and culprit stamp
    // the use_pack_process_cb boundary applies. Non-capturing → fn pointers.
    xi::set_cap_fault_note([](const char* instance, const char* why) {
        note_instance_crash_(instance, why);
    });
    xi::set_cap_precall([](const char* instance, const char* plugin) {
        stamp_culprit_(instance, std::string(plugin ? plugin : ""));
    });

    // P1-3: forward plugin/script host_api->log to the operator channel. stderr is
    // unwatched on an unattended PC, so a plugin's WARN/ERROR self-diagnostics used
    // to vanish. WARN/ERROR escalate to the WS log channel (clients see them live +
    // re-pull via cmd:recent_errors); ERROR also lands in the recent-errors ring.
    // DEBUG/INFO stay stderr-only (already printed in make_host_api) to avoid
    // flooding the WS log. Non-capturing → converts to LogSinkFn. Thread-safe:
    // send_text + push_recent_error may be called from any dispatch worker.
    xi::log_sink() = [](int32_t level, const char* msg, int64_t /*ts_ms*/) {
        if (level < 2 || !msg) return;                 // only WARN(2)/ERROR(3) escalate
        if (auto* s = g_eng.srv_for_bp.load(std::memory_order_acquire)) {
            xp::LogMsg lm; lm.level = (level >= 3) ? "error" : "warn"; lm.msg = msg;
            s->send_text(lm.to_json());
        }
        if (level >= 3) push_recent_error("plugin", msg);
    };

    // P2.4 watchdog. Always-on monitor thread; acts when any in-flight inspect
    // (wd_arm slot) overruns its deadline. Two-phase, now per-worker-aware:
    //   Phase 1 — cooperative: arm the script's EPOCH-scoped cancel
    //     (set_global_cancel(1)); xi::ops poll xi::cancellation_requested() and
    //     bail. 1000 ms grace (big ops — 20 MP gaussian, matchTemplate, contour
    //     walks — need a few hundred ms to finish their current chunk; 100 ms
    //     tripped healthy scripts). The arm targets only inspects ALREADY in
    //     flight at trip time (ticket below the high-water snapshot): under N>1
    //     it aborts every currently-running frame this round — the intended
    //     "something's wedged, bail" signal — but a FRESH frame the pool starts
    //     during the grace draws a higher ticket and is NOT cancelled, so one
    //     slow frame no longer poisons ~a second of unrelated frames. Healthy
    //     workers re-run next tick. (Pre-fix the flag was a held global bool, so
    //     every heavy frame dispatched in the grace window aborted spuriously —
    //     core-bug-hunt 2026-06 #12.)
    //   Phase 2 — hard trip: if any slot is STILL overrun after the grace, the
    //     script ignored cooperative cancel. We do NOT TerminateThread — a kill
    //     mid process() would leak the per-instance lock (deadlocking that
    //     instance) and risk heap corruption. The process is unrecoverable, so
    //     the backend EXITS; the FE supervisor respawns a clean one (and drives
    //     the line safe). Run without an FE => backend stays down by design.
    g_eng.watchdog_run = true;
    g_eng.watchdog_thread = std::thread([&srv]() {
        auto now_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        // Snapshot of the slot deadlines that were overrun when we armed a
        // cooperative cancel. After the grace we hard-trip ONLY if one of THESE
        // same inspects is still stuck (same slot still holds the same deadline)
        // — i.e. it ignored the cooperative cancel it was actually targeted by.
        // A different inspect overrunning by then (a fresh frame that started
        // during the grace, which the epoch-scoped cancel deliberately did NOT
        // target) is left for the next loop iteration to give its OWN
        // cooperative round, rather than being hard-killed without warning.
        int64_t wd_snap[WD_SLOTS];
        while (g_eng.watchdog_run.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            int64_t now = now_ms();
            bool any_overran = false;
            for (int i = 0; i < WD_SLOTS; ++i) {
                int64_t dl = g_eng.wd_deadlines[i].load();
                if (dl != 0 && now >= dl) { wd_snap[i] = dl; any_overran = true; }
                else                       { wd_snap[i] = 0; }
            }
            if (!any_overran) continue;

            // Phase 1: cooperative cancel + grace. Log the attempt so the
            // escalation is observable (and a hard trip can be proven to have
            // tried the soft cancel first, not jumped straight to the kill).
            std::fprintf(stderr,
                "[xinsp2] watchdog: inspect overran %dms — requesting cooperative cancel\n",
                g_eng.watchdog_ms.load());
            {
                std::lock_guard<std::mutex> lk(g_eng.script_mu);
                if (g_eng.script.set_global_cancel) g_eng.script.set_global_cancel(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            // Did every inspect we TARGETED return? (Its slot is now free or
            // re-armed by a different inspect with a different deadline.) Match
            // on slot index AND deadline value so a fresh inspect reusing the
            // slot is not mistaken for the original stuck one.
            bool still_stuck = false;
            for (int i = 0; i < WD_SLOTS; ++i) {
                if (wd_snap[i] != 0 && g_eng.wd_deadlines[i].load() == wd_snap[i]) {
                    still_stuck = true; break;
                }
            }
            if (!still_stuck) {
                {
                    std::lock_guard<std::mutex> lk(g_eng.script_mu);
                    if (g_eng.script.set_global_cancel) g_eng.script.set_global_cancel(0);
                }
                int n = ++g_eng.watchdog_trips;
                std::fprintf(stderr,
                    "[xinsp2] watchdog tripped (#%d) — script honoured cooperative cancel\n", n);
                emit_error_log(srv,
                    "watchdog tripped — inspect exceeded "
                    + std::to_string(g_eng.watchdog_ms.load())
                    + "ms; cooperative cancel succeeded");
                continue;
            }

            // Phase 2: hard trip — exit for FE respawn (see header above).
            ++g_eng.watchdog_trips;
            // Health contract: an unrecoverable wedge → `fault`. Best-effort push
            // before the exit (the FE will respawn into a fresh `boot`).
            xi::health().set_state(xi::SysState::Fault);
            std::fprintf(stderr,
                "[xinsp2] watchdog HARD trip - inspect exceeded %dms and ignored "
                "cooperative cancel; exiting for supervisor respawn (rc=0x%04X)\n",
                g_eng.watchdog_ms.load(), WATCHDOG_EXIT_CODE);
            emit_error_log(srv,
                "watchdog HARD trip — inspect exceeded "
                + std::to_string(g_eng.watchdog_ms.load())
                + "ms and ignored cooperative cancel; backend exiting for respawn");
            std::fflush(stderr);
            std::fflush(stdout);
            // H7: if a worker is mid-write_minidump, let its dump land before we
            // hard-exit (bounded — never blocks the respawn forever).
            xi::crash::await_dump(10000);
            // _Exit: skip static destructors / atexit — a wedged worker may hold
            // locks those would block on. The FE sees the exit and respawns.
            std::_Exit(WATCHDOG_EXIT_CODE);
        }
    });
    // Stop+join the always-on watchdog on ANY exit from here down — the bind-fail
    // `return 1` just below and the debug `--hang-*` `return 0`s all sit AFTER the
    // thread was spawned. A joinable std::thread destroyed at static teardown (the
    // file-scope g_eng.watchdog_thread) invokes std::terminate, which the still-armed
    // crash filter turns into a spurious minidump + abnormal exit — the FE reads a
    // routine port-in-use as a backend CRASH. The normal epilogue joins too; this
    // then no-ops (joinable()==false).
    struct WatchdogJoiner {
        ~WatchdogJoiner() {
            g_eng.watchdog_run = false;
            if (g_eng.watchdog_thread.joinable()) g_eng.watchdog_thread.join();
        }
    } wd_joiner_;
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
    if (std::string project = xi::cli::parse_str_flag(argc, argv, "--project"); !project.empty()) {
        std::string script = xi::cli::parse_str_flag(argc, argv, "--script");
        int autostart_fps  = xi::cli::parse_autostart_fps(argc, argv);
        // --working-copy: open via a <project>/.xinsp_work scratch. On a crash
        // respawn the FE passes the same flag; the scratch still exists, so the
        // backend resumes the last in-progress settings instead of reverting to
        // the pristine project. See docs/guides/deploy.md.
        bool working_copy = xi::cli::has_flag(argc, argv, "--working-copy");

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
            // "<folder>/inspect.cpp"), so a script-less project resolves to a
            // path that may not exist — treat a missing script FILE as open-only
            // rather than firing a doomed compile.
            if (!script.empty()) {
                std::filesystem::path sp(script);
                if (sp.is_relative()) sp = std::filesystem::path(project) / sp;
                script = sp.string();
            } else {
                script = g_eng.plugin_mgr.project().script_path;
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
                if (!g_eng.script.ok()) {
                    degraded = true;
                    std::fprintf(stderr,
                        "[xinsp2] autostart: degraded - script failed to compile/load; "
                        "line will NOT inspect (port stays up for an operator to recompile)\n");
                } else if (autostart_fps != 0) {
                    // >0 = timer at N fps; <0 = trigger-only (no timer). The fps
                    // value passes through to cmd:start, which treats <=0 as
                    // trigger-only. (Keep the ">0" wording as "start N fps" — qa_func
                    // FE-E9 pins that line.)
                    if (autostart_fps > 0)
                        std::fprintf(stderr, "[xinsp2] autostart: start %d fps\n", autostart_fps);
                    else
                        std::fprintf(stderr, "[xinsp2] autostart: start (trigger-only, no timer)\n");
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
        if (xi::cli::has_flag(argc, argv, "--hang-before-ready")) {
            std::fprintf(stderr, "[xinsp2] autostart: --hang-before-ready (debug) — "
                                 "hanging before ready\n");
            std::fflush(stderr);
            while (!g_eng.should_exit.load()) {
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
    std::string hb_path = xi::cli::parse_str_flag(argc, argv, "--heartbeat-file");
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
    if (xi::cli::has_flag(argc, argv, "--hang-after-ready")) {
        std::fprintf(stderr, "[xinsp2] --hang-after-ready (debug) — wedging the serving loop\n");
        std::fflush(stderr);
        while (!g_eng.should_exit.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return 0;
    }

    // T2: catch abrupt console exits (window close / Ctrl+C / logoff / shutdown)
    // so they run the controlled teardown instead of a bare ExitProcess. Installed
    // here — after all startup is done and the teardown machinery (srv, pool,
    // project) is live — so the handler's flip-and-wait always has a real loop to
    // service it.
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(console_ctrl_handler_, TRUE))
        std::fprintf(stderr, "[xinsp2] warning: SetConsoleCtrlHandler failed (%lu)\n",
                     (unsigned long)GetLastError());
#endif

    int64_t hb_last_ms = 0;
    while (!g_eng.should_exit.load() && srv.is_running()) {
        srv.poll(100);
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - hb_last_ms >= 1000) { write_heartbeat(); hb_last_ms = now; }
    }

    g_eng.watchdog_run = false;
    if (g_eng.watchdog_thread.joinable()) g_eng.watchdog_thread.join();   // join before teardown nulls srv
    // Controlled teardown before `srv` (a main() local captured by the bus sink +
    // g_eng.srv_for_bp) leaves scope, and while the ImagePool/TriggerBus singletons
    // are still alive — covers exits that didn't go through cmd:shutdown (e.g.
    // g_eng.should_exit flipped elsewhere). Single source of truth; idempotent with
    // the shutdown handler. Runs BEFORE srv.stop() so the pool's workers are
    // joined (no emit) before the server goes away.
    controlled_shutdown_teardown_();
    srv.stop();
    std::fprintf(stderr, "[xinsp2] shutdown complete\n");
    return 0;
}
