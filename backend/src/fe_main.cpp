//
// fe_main.cpp — xinsp-fe.exe, the frontend supervisor.
//
// Process isolation was removed (2026-05): a hard plugin crash takes the whole
// backend (BE) compute core down. The FE is the replacement safety net. It:
//   1. spawns xinsp-backend.exe (with --project/--autostart-fps so the BE runs
//      a line headlessly — no WS client needed; see service_main.cpp autostart),
//   2. monitors it (process-exit + a shallow TCP probe of the WS port),
//   3. the instant the BE dies, reads its crash report for forensics + records
//      the death to the crash history,
//   4. respawns the BE with the same rate-limit the VS Code extension uses
//      (5 / 60s, 1.5s backoff); if that cap is exceeded it gives up and waits
//      for a human.
//
// (The "drive the production line to a safe state on BE crash" action moved to
// the comms plugin's own sidecar process — removed from core/FE 2026-06.)
//
// Windows-only today: process spawn/wait, Job Object, console-ctrl, and the TCP
// probe are Win32. The crash-event model + crash-log parsing stay portable. TODO(linux) markers below; see docs/roadmap/linux-port.md.
//
#include <xi/xi_clock.hpp>            // xi::wall_ms / xi::mono_ms (single clock home)
#include <xi/xi_be_exit.hpp>
#include <xi/xi_crash_report.hpp>     // xi::enrich_from_crash_report (unit-tested)
#include <xi/xi_crash_history.hpp>    // xi::CrashHistory (structured BE-death JSONL)
#include <xi/xi_fe_status.hpp>        // xi::FeStatus (live status snapshot -> JSON file)
#include <xi/xi_be_health.hpp>        // xi::BeHealth + parse_be_health (BE health mirror read)
#include <xi/xi_respawn_policy.hpp>   // xi::RespawnTracker + PluginFaultTracker (unit-tested)
#include <xi/xi_sha256.hpp>          // xi::sha256::sha256_file (quarantine cache key)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <yyjson.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "Ws2_32.lib")
#endif

namespace fs = std::filesystem;

// ----------------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------------
struct FeConfig {
    std::string backend_exe;       // resolved path to xinsp-backend.exe
    int         port           = 7823;
    std::string project;           // --project (required to be useful)
    std::string script;            // --script (optional; BE defaults from project.json)
    int         autostart_fps  = 10;
    std::vector<std::string> plugins_dirs;
    std::string be_log;            // where the BE's stdout/stderr is captured
    // Respawn policy: latch safe after `respawn_max` CONSECUTIVE failures; the
    // counter resets once the backend stays healthy for `respawn_reset_ms`
    // (genuine recovery). Catches fast AND slow crash-loops.
    int         respawn_max        = 5;
    int         respawn_backoff_ms = 1500;
    int         respawn_reset_ms   = 30000;
    // Liveness probe.
    int         probe_interval_ms  = 1000;
    int         probe_fail_max     = 5;   // consecutive failures (proc alive) -> unresponsive
    // Boot-readiness gate: when running a --project, the BE binds its port then
    // open/compiles for several seconds before serving (port-up != ready). The
    // FE waits for the "autostart: ready" marker in be.log before declaring the
    // line healthy; if the BE is alive but never reaches ready within this
    // budget it's a boot hang -> respawn. 0 disables the gate.
    int         boot_timeout_ms    = 60000;
    // Serve-time liveness: the backend writes a monotonic counter to this file
    // from its serving loop; if it stops advancing while the process is alive
    // and the port still accepts, the backend is wedged (bound but not serving)
    // -> respawn. Default derived from be_log; 0 stale = off.
    std::string heartbeat_file;
    // 15 s, not 8 s: a mid-run compile_and_load blocks the backend's serving loop
    // (cl.exe is synchronous, 3-5 s warm and can exceed 8 s on a cold toolchain),
    // which legitimately stalls the heartbeat. The window must be safely above
    // one cold compile so a real recompile isn't mistaken for a serving wedge; a
    // genuine wedge is still caught, just ~7 s later.
    int         heartbeat_stale_ms = 15000;
    // BE health mirror: the backend writes its canonical top-level health
    // (running/degraded/fault …) to this file on every health_changed, and the FE
    // reflects it in fe_status WITHOUT holding the single WS client slot the
    // HMI/extension needs (docs/internals/fe-be.md). Default derived from be_log,
    // like heartbeat_file; empty disables it.
    std::string health_file;
    // Per-inspect watchdog budget passed to the backend (--watchdog=N). The
    // serving-loop heartbeat above only catches a wedged SERVING loop; a dispatch
    // WORKER deadlocked inside a plugin's process() keeps the serving loop (and so
    // the heartbeat) alive, so without this an unattended line could silently stop
    // inspecting while the FE shows healthy. The backend's watchdog arms per
    // inspect (covers continuous workers + cmd:run) and, on overrun, cooperatively
    // cancels then exits — the FE respawns. Default ON and generous so a real long
    // frame isn't killed; a genuine hang is caught within the budget. 0 disables
    // (e.g. a dev running deliberately long inspects). The BE default stays off, so
    // this is the supervised-deployment fail-safe specifically.
    int         watchdog_ms = 60000;
    // Extra args appended verbatim to the spawned backend's command line
    // (--be-arg=..., repeatable). Lets an operator pass BE flags through the FE.
    std::vector<std::string> be_args;
    // Working copy: pass --working-copy to the backend so it edits a
    // <project>/.xinsp_work scratch. On a crash respawn the same flag is passed
    // and the backend resumes the scratch (settings survive). See
    // docs/guides/deploy.md.
    bool        working_copy = false;
    // Crash history: every BE death appends one structured JSONL record (death
    // reason, rc, parsed forensics, consecutive count, cap-hit) — the FE is the
    // only process that survives every death, so it owns the timeline. Default
    // lands next to be_log; empty disables. preserve_dumps copies each referenced
    // .dmp (+ .json) into preserve_dir so a %TEMP% cleanup can't lose it.
    // See docs/internals/fe-be.md.
    std::string crash_history;     // JSONL path (default: <be_log dir>/crash-history.jsonl)
    bool        preserve_dumps = false;
    std::string preserve_dir;      // default: <be_log dir>/crash-dumps
    // Status channel: the FE rewrites this small JSON file (atomically) on every
    // transition so the UI / an operator HMI can read the line's TRUE state
    // (safe/healthy, reason, respawn budget, last death) instead of
    // inferring "down" from a WS disconnect. Default next to be_log; empty/"-" off.
    std::string status_file;       // default: <be_log dir>/fe-status.json
};

static std::string arg_value(int argc, char** argv, const char* flag) {
    std::string eq = std::string(flag) + "=";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind(eq, 0) == 0) return a.substr(eq.size());
        if (a == flag && i + 1 < argc) return argv[i + 1];
    }
    return {};
}

static bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == flag) return true;
    return false;
}

// Walk up from the FE exe dir looking for a sibling exe by base name (mirrors
// service_main.cpp's parent-walk for include/plugins dirs).
static std::string discover_sibling_exe(const char* base) {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    fs::path dir = fs::path(std::string(buf, n)).parent_path();
    std::string exe = std::string(base) + ".exe";
#else
    fs::path dir = fs::current_path();
    std::string exe = base;
#endif
    fs::path p = dir;
    for (int i = 0; i < 6; ++i) {
        if (fs::exists(p / exe)) return (p / exe).string();
        if (!p.has_parent_path() || p.parent_path() == p) break;
        p = p.parent_path();
    }
    return (dir / exe).string();  // best guess: sibling of the FE
}
static std::string discover_backend_exe() { return discover_sibling_exe("xinsp-backend"); }

static FeConfig load_config(int argc, char** argv) {
    FeConfig c;

    // Optional fe.json (CLI flags override it). Keys mirror the flags.
    std::string cfg_path = arg_value(argc, argv, "--config");
    if (cfg_path.empty() && fs::exists("fe.json")) cfg_path = "fe.json";
    if (!cfg_path.empty()) {
        std::ifstream f(cfg_path);
        std::stringstream ss; ss << f.rdbuf();
        std::string cfg_text = ss.str();
        yyjson_doc* doc = yyjson_read(cfg_text.c_str(), cfg_text.size(), 0);
        yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (root) {
            auto str = [&](const char* k, std::string& dst) {
                yyjson_val* v = yyjson_obj_get(root, k);
                if (yyjson_is_str(v) && yyjson_get_str(v)) dst = yyjson_get_str(v);
            };
            auto num = [&](const char* k, int& dst) {
                yyjson_val* v = yyjson_obj_get(root, k);
                if (yyjson_is_num(v)) dst = (int)yyjson_get_num(v);
            };
            str("backend", c.backend_exe);
            num("port", c.port);
            str("project", c.project);
            str("script", c.script);
            num("autostart_fps", c.autostart_fps);
            str("be_log", c.be_log);
            str("crash_history", c.crash_history);
            str("preserve_dir", c.preserve_dir);
            str("status_file", c.status_file);
            if (yyjson_val* pdmp = yyjson_obj_get(root, "preserve_dumps"); yyjson_is_bool(pdmp))
                c.preserve_dumps = yyjson_get_bool(pdmp);
            if (yyjson_val* wc = yyjson_obj_get(root, "working_copy"); yyjson_is_bool(wc))
                c.working_copy = yyjson_get_bool(wc);
            num("respawn_max", c.respawn_max);
            num("respawn_reset_ms", c.respawn_reset_ms);
            num("respawn_backoff_ms", c.respawn_backoff_ms);
            if (yyjson_val* pd = yyjson_obj_get(root, "plugins_dirs"); yyjson_is_arr(pd)) {
                size_t _i, _n; yyjson_val* it;
                yyjson_arr_foreach(pd, _i, _n, it)
                    if (yyjson_is_str(it) && yyjson_get_str(it)) c.plugins_dirs.push_back(yyjson_get_str(it));
            }
        }
        yyjson_doc_free(doc);
    }

    // CLI overrides.
    if (auto v = arg_value(argc, argv, "--backend");        !v.empty()) c.backend_exe = v;
    if (auto v = arg_value(argc, argv, "--port");           !v.empty()) try { c.port = std::stoi(v); } catch (...) {}
    if (auto v = arg_value(argc, argv, "--project");        !v.empty()) c.project = v;
    if (auto v = arg_value(argc, argv, "--script");         !v.empty()) c.script = v;
    if (auto v = arg_value(argc, argv, "--autostart-fps");  !v.empty()) try { c.autostart_fps = std::stoi(v); } catch (...) {}
    if (auto v = arg_value(argc, argv, "--be-log");         !v.empty()) c.be_log = v;
    if (auto v = arg_value(argc, argv, "--boot-timeout-ms"); !v.empty()) try { c.boot_timeout_ms = std::stoi(v); } catch (...) {}
    if (auto v = arg_value(argc, argv, "--heartbeat-file");  !v.empty()) c.heartbeat_file = v;
    if (auto v = arg_value(argc, argv, "--health-file");     !v.empty()) c.health_file = v;
    if (has_flag(argc, argv, "--no-health-file")) c.health_file = "-";  // explicit off
    if (auto v = arg_value(argc, argv, "--heartbeat-stale-ms"); !v.empty()) try { c.heartbeat_stale_ms = std::stoi(v); } catch (...) {}
    if (auto v = arg_value(argc, argv, "--watchdog-ms"); !v.empty()) try { c.watchdog_ms = std::stoi(v); } catch (...) {}
    if (auto v = arg_value(argc, argv, "--crash-history"); !v.empty()) c.crash_history = v;
    if (auto v = arg_value(argc, argv, "--preserve-dir");  !v.empty()) c.preserve_dir = v;
    if (auto v = arg_value(argc, argv, "--status-file");   !v.empty()) c.status_file = v;
    if (has_flag(argc, argv, "--no-status-file")) c.status_file = "-";  // explicit off
    if (has_flag(argc, argv, "--preserve-dumps")) c.preserve_dumps = true;
    if (has_flag(argc, argv, "--no-crash-history")) c.crash_history = "-";  // explicit off
    if (has_flag(argc, argv, "--working-copy")) c.working_copy = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--plugins-dir=", 0) == 0) c.plugins_dirs.push_back(a.substr(14));
        else if (a == "--plugins-dir" && i + 1 < argc) c.plugins_dirs.push_back(argv[++i]);
        else if (a.rfind("--be-arg=", 0) == 0) c.be_args.push_back(a.substr(9));
        else if (a == "--be-arg" && i + 1 < argc) c.be_args.push_back(argv[++i]);
    }

    if (c.backend_exe.empty()) c.backend_exe = discover_backend_exe();
    if (c.be_log.empty())      c.be_log = (fs::temp_directory_path() / "xinsp2-fe-be.log").string();
    if (c.heartbeat_file.empty()) c.heartbeat_file = c.be_log + ".hb";
    if (c.health_file == "-")        c.health_file.clear();   // explicitly disabled
    else if (c.health_file.empty())  c.health_file = c.be_log + ".health";
    // Crash history defaults next to be_log; "-" means explicitly disabled.
    if (c.crash_history == "-") {
        c.crash_history.clear();
    } else if (c.crash_history.empty()) {
        c.crash_history = (fs::path(c.be_log).parent_path() / "crash-history.jsonl").string();
    }
    if (c.preserve_dumps && c.preserve_dir.empty())
        c.preserve_dir = (fs::path(c.be_log).parent_path() / "crash-dumps").string();
    // Status file defaults next to be_log; "-" means explicitly disabled.
    if (c.status_file == "-") {
        c.status_file.clear();
    } else if (c.status_file.empty()) {
        c.status_file = (fs::path(c.be_log).parent_path() / "fe-status.json").string();
    }
    return c;
}

// Wall clock — human-facing timestamps (status / crash JSON). Monotonic clock —
// the supervisor's deadlines/intervals (boot timeout, heartbeat staleness,
// healthy-for, respawn windows) which must not jump on an NTP/DST correction.
// See xi_clock.hpp for the contract. Thin aliases keep the call sites readable.
static int64_t now_ms()    { return xi::wall_ms(); }
static int64_t steady_ms() { return xi::mono_ms(); }

// Cheap substring scan of the BE log (used by the boot-readiness gate to spot
// the "autostart: ready" marker). Reads the whole small log each call.
static bool log_contains(const std::string& path, const char* needle) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    return ss.str().find(needle) != std::string::npos;
}

// Read the backend's heartbeat counter; -1 if the file is missing/unparseable
// (treated as "no advance" by the staleness check).
static long long read_heartbeat(const std::string& path) {
    std::ifstream f(path);
    if (!f) return -1;
    long long v = -1;
    f >> v;
    return f ? v : -1;
}

// Slurp the BE health-mirror file (tiny JSON). Empty on missing/unreadable — the
// caller then leaves the last-known BE health in place for this poll.
static std::string read_health_mirror(const std::string& path) {
    if (path.empty()) return {};
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// Crash-report parsing (read the BE's log for the minidump path it printed,
// then the sibling .json) lives in xi/xi_crash_report.hpp as
// xi::enrich_from_crash_report — extracted so it can be unit-tested
// (backend/tests/test_qa_edge.cpp) against crafted fixtures. The FE just calls it.

// Part III G2.2 — quarantine the plugin a death was attributed to by writing G1's
// .xi_certify.json into its folder with verdict "quarantined", keyed by the DLL's
// CURRENT content hash. The backend's scan_plugins gate then disables that plugin
// on the next open (keeping the line up), and a later DLL rebuild — a new hash —
// auto-clears it (G2.3 / G1.2). One mechanism, two writers (Invariant §20.3).
//
// The FE writes the schema directly rather than calling certify::write_cache: that
// header pulls the ImagePool/OpenCV stack the FE deliberately doesn't link. The
// {dll, sha256, verdict} schema is the cross-process contract certify::read_cache
// parses. Best-effort + total: a read-only folder / vanished DLL just means the
// plugin isn't quarantined this time (the supervisor still respawns).
static bool quarantine_plugin(const std::string& folder, const std::string& dll) {
    if (folder.empty() || dll.empty()) return false;
    std::string dll_path = (fs::path(folder) / dll).string();
    std::string sha = xi::sha256::sha256_file(dll_path);
    if (sha.empty()) return false;   // DLL gone/unreadable — no hash to pin a verdict to
    std::string js = "{\"dll\":\"" + dll + "\",\"sha256\":\"" + sha +
                     "\",\"verdict\":\"quarantined\"}\n";
    std::string cache = (fs::path(folder) / ".xi_certify.json").string();
    std::ofstream f(cache, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << js;
    return (bool)f;
}

#ifdef _WIN32
// ----------------------------------------------------------------------------
// Win32 supervisor
// ----------------------------------------------------------------------------
static std::atomic<bool> g_stop{false};   // set by console-ctrl handler

// Write the FE status snapshot atomically: a reader (the UI) must never observe a
// half-written file. Write a sibling .tmp then MoveFileEx-replace it (atomic on
// the same volume). Best-effort: a failed status write must never take the
// supervisor down. TODO(linux): write tmp + std::filesystem::rename (POSIX rename
// atomically replaces; MoveFileEx is the Windows equivalent).
static void publish_status(const FeConfig& c, xi::FeStatus& st) {
    if (c.status_file.empty()) return;
    st.ts_ms = now_ms();
    std::string json = st.render();
    std::string tmp  = c.status_file + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << json;
        if (!f) return;
    }
    if (!MoveFileExA(tmp.c_str(), c.status_file.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        fs::rename(tmp, c.status_file, ec);   // best-effort fallback
    }
}

static BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

// Shallow "is the WS port accepting?" probe — mirrors the Python driver's
// port_open. Not a deep heartbeat (that needs a WS client; Phase 2); it only
// tells us the BE's accept loop is alive.
static bool port_open(int port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr);
    // Non-blocking connect with a short timeout so a hung BE doesn't stall us.
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    bool ok = false;
    if (::connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
        ok = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
        timeval tv{0, 250 * 1000};  // 250 ms
        ok = ::select(0, nullptr, &wf, nullptr, &tv) > 0;
    }
    closesocket(s);
    return ok;
}

struct Spawned {
    PROCESS_INFORMATION pi{};
    HANDLE log = INVALID_HANDLE_VALUE;
    bool ok = false;
};

static std::string build_cmdline(const FeConfig& c) {
    // CreateProcessA wants a mutable, fully-quoted command line.
    std::string cl = "\"" + c.backend_exe + "\"";
    cl += " --port=" + std::to_string(c.port);
    if (!c.project.empty()) cl += " --project=\"" + c.project + "\"";
    if (!c.script.empty())  cl += " --script=\"" + c.script + "\"";
    if (c.autostart_fps > 0) cl += " --autostart-fps=" + std::to_string(c.autostart_fps);
    for (auto& d : c.plugins_dirs) cl += " --plugins-dir=\"" + d + "\"";
    if (c.heartbeat_stale_ms > 0 && !c.heartbeat_file.empty())
        cl += " --heartbeat-file=\"" + c.heartbeat_file + "\"";
    // Tell the BE where to mirror its canonical health so the FE can reflect it in
    // fe_status without opening a WS client (the single client slot stays free).
    if (!c.health_file.empty())
        cl += " --health-file=\"" + c.health_file + "\"";
    // Fail-safe for an unattended line: arm the backend's per-inspect watchdog so a
    // worker wedged in a plugin's process() is caught + respawned (the heartbeat
    // can't see it). 0 disables.
    if (c.watchdog_ms > 0)    cl += " --watchdog=" + std::to_string(c.watchdog_ms);
    if (c.working_copy)       cl += " --working-copy";
    for (auto& a : c.be_args)      cl += " " + a;   // verbatim passthrough
    return cl;
}

static Spawned spawn_backend(const FeConfig& c, HANDLE job) {
    Spawned sp;
    // Truncate + open the BE log; inheritable so the child writes into it and
    // we can parse the crash line afterwards.
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    sp.log = CreateFileA(c.be_log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOA si{}; si.cb = sizeof(si);
    if (sp.log != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = sp.log;
        si.hStdError  = sp.log;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    }

    std::string cl = build_cmdline(c);
    std::vector<char> mut(cl.begin(), cl.end()); mut.push_back('\0');

    fs::path wd = fs::path(c.backend_exe).parent_path();
    sp.ok = CreateProcessA(c.backend_exe.c_str(), mut.data(), nullptr, nullptr,
                           /*bInheritHandles=*/TRUE, CREATE_SUSPENDED,
                           nullptr, wd.string().c_str(), &si, &sp.pi) != 0;
    if (!sp.ok) {
        std::fprintf(stderr, "[xinsp-fe] CreateProcess failed (%lu): %s\n",
                     GetLastError(), c.backend_exe.c_str());
        if (sp.log != INVALID_HANDLE_VALUE) CloseHandle(sp.log);
        return sp;
    }
    // Assign to the job BEFORE resuming so the child can never escape the
    // kill-on-close guarantee even if it spawns instantly.
    if (job) AssignProcessToJobObject(job, sp.pi.hProcess);
    ResumeThread(sp.pi.hThread);
    return sp;
}

// Force-kill the backend and CONFIRM death before the caller may respawn (H1).
// TerminateProcess is async: the handle signals only after every thread has
// terminated, which a thread wedged in an uninterruptible kernel op (blocking
// driver I/O, a page fault on a stalled mmap, an FS write to a disk under RT8
// record-to-disk pressure) can delay past a single 5 s wait. Retry the terminate;
// return true only once the handle is signaled — and read the REAL exit code THEN,
// not the STILL_ACTIVE (259) a live process reports. Return false if it is still
// alive after the budget: the caller must NOT respawn onto the same port +
// .xinsp_work dir, or two live backends double-serve and corrupt shared state.
static bool force_kill_be(HANDLE hProcess, DWORD* exit_code) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        TerminateProcess(hProcess, 1);
        if (WaitForSingleObject(hProcess, 5000) == WAIT_OBJECT_0) {
            GetExitCodeProcess(hProcess, exit_code);
            return true;
        }
    }
    return false;
}

static int run_supervisor(const FeConfig& c) {
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    std::fprintf(stderr, "[xinsp-fe] supervisor up: backend=%s port=%d project=%s fps=%d\n",
                 c.backend_exe.c_str(), c.port, c.project.c_str(), c.autostart_fps);
    std::fprintf(stderr, "[xinsp-fe] BE log -> %s\n", c.be_log.c_str());

    // Structured crash history: one JSONL record per BE death (the FE is the only
    // process that survives every death, so it owns the timeline). The BE writes
    // the dumps; the FE only references them (+ optionally preserves them).
    xi::CrashHistory history(c.crash_history, c.preserve_dir);
    if (history.enabled()) {
        std::fprintf(stderr, "[xinsp-fe] crash history -> %s%s\n", c.crash_history.c_str(),
                     c.preserve_dir.empty() ? "" : (" (dumps -> " + c.preserve_dir + ")").c_str());
    }

    // Live status snapshot, republished (atomically) on every transition so the
    // UI reads the line's TRUE state instead of inferring it from a WS drop.
    xi::FeStatus st;
    st.respawn_max   = c.respawn_max;
    st.port          = c.port;
    st.working_copy  = c.working_copy;
    st.crash_history = c.crash_history;
    st.state         = "starting";
    if (!c.status_file.empty())
        std::fprintf(stderr, "[xinsp-fe] status -> %s\n", c.status_file.c_str());
    publish_status(c, st);

    // Job object: kill the BE if the FE dies, so it can never orphan.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl{};
        jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof(jl));
    }

    xi::RespawnTracker resp;         // consecutive-failure cap (latches "down")
    xi::PluginFaultTracker plugin_faults;  // G2.2 per-plugin crash budget (quarantine)
    int  rc = 0;

    while (!g_stop.load()) {
        Spawned sp = spawn_backend(c, job);
        if (!sp.ok) {
            // A spawn failure isn't necessarily permanent — an AV scan holding the
            // exe open (sharing violation), a transient ENOMEM, a momentary path
            // glitch. Exiting the supervisor here would take an unattended line down
            // FOREVER on a blip. Treat it like a death: record + backoff + retry,
            // governed by the SAME consecutive-failure cap so a genuinely broken
            // install still latches "down" instead of spinning.
            xi::BeExitEvent ev; ev.reason = xi::BeExitReason::BackendExit;
            ev.ts_ms = now_ms(); ev.backend_rc = -1;
            bool cap_hit = resp.note_death(c.respawn_max);
            history.record(ev, resp.consecutive, cap_hit);
            st.state = "down"; st.reason = "BackendSpawnFailed"; st.backend_pid = 0;
            st.consecutive = resp.consecutive;
            st.set_event(ev); publish_status(c, st);
            if (g_stop.load()) { rc = 1; break; }
            if (cap_hit) {
                std::fprintf(stderr, "[xinsp-fe] backend spawn failed %d consecutive times "
                             "(cap %d) - staying down; manual restart required\n",
                             resp.consecutive, c.respawn_max);
                st.latched = true; st.reason = "RespawnLimitExceeded"; publish_status(c, st);
                rc = 2;
                break;
            }
            std::fprintf(stderr, "[xinsp-fe] backend spawn failed (failure %d/%d); "
                         "retrying after %dms\n", resp.consecutive, c.respawn_max, c.respawn_backoff_ms);
            Sleep((DWORD)c.respawn_backoff_ms);
            continue;
        }
        std::fprintf(stderr, "[xinsp-fe] backend up pid=%lu\n", sp.pi.dwProcessId);
        st.backend_pid = (int)sp.pi.dwProcessId;
        publish_status(c, st);

        // ---- monitor this instance ----
        int   probe_fails = 0;
        bool  healthy_announced = false;
        // Boot-readiness gate: until the BE reaches "autostart: ready", port-up
        // only means "bound", not "serving". No project -> nothing to wait for.
        bool    ready_seen = c.project.empty();
        int64_t spawn_ms   = steady_ms();
        // Serve-time heartbeat tracking (this instance).
        long long hb_last_val = -2;
        int64_t   hb_last_change_ms = 0;
        bool      hb_armed = false;
        int64_t   healthy_since = 0;   // when this instance first went healthy
        DWORD exit_code = 0;
        bool  kill_confirmed = true;   // normal WAIT_OBJECT_0 exit is already dead
        xi::BeExitReason death_reason = xi::BeExitReason::BackendExit;
        for (;;) {
            DWORD w = WaitForSingleObject(sp.pi.hProcess, (DWORD)c.probe_interval_ms);
            if (w == WAIT_OBJECT_0) {                       // BE exited
                GetExitCodeProcess(sp.pi.hProcess, &exit_code);
                death_reason = xi::BeExitReason::BackendExit;
                break;
            }
            if (g_stop.load()) break;

            bool port = port_open(c.port);

            // Boot gate: the BE binds its port then open/compiles synchronously
            // for seconds before serving. A connect probe can't tell "booting"
            // from "serving" (both accept TCP). Wait for the readiness marker;
            // if the BE is alive but never reaches it in time, that's a boot
            // hang — go safe + respawn rather than reporting a false "healthy".
            if (!ready_seen) {
                if (port && log_contains(c.be_log, "autostart: ready")) {
                    ready_seen = true;   // fall through to healthy handling
                } else if (log_contains(c.be_log, "autostart: degraded")) {
                    // The BE opened + bound its port but its script failed to
                    // compile/load — it's listening but cannot inspect. Don't
                    // wait out the boot timeout; treat it as a failed boot now.
                    std::fprintf(stderr, "[xinsp-fe] backend reported autostart degraded "
                                 "(script did not load); killing for respawn\n");
                    death_reason = xi::BeExitReason::BootTimeout;
                    kill_confirmed = force_kill_be(sp.pi.hProcess, &exit_code);
                    break;
                } else if (c.boot_timeout_ms > 0 && steady_ms() - spawn_ms > c.boot_timeout_ms) {
                    std::fprintf(stderr, "[xinsp-fe] backend did not reach 'autostart: ready' "
                                 "within %d ms (boot hang); killing for respawn\n",
                                 c.boot_timeout_ms);
                    death_reason = xi::BeExitReason::BootTimeout;
                    kill_confirmed = force_kill_be(sp.pi.hProcess, &exit_code);
                    break;
                } else {
                    continue;   // still booting — don't declare healthy or count fails
                }
            }

            // Ready (or no gate) — normal liveness handling.
            if (port) {
                probe_fails = 0;
                // Announce liveness once per backend instance so a healthy line
                // has a positive "inspector up" signal in the log, not just
                // silence. (Without this a never-crashed start logged nothing.)
                if (!healthy_announced) {
                    healthy_announced = true;
                    std::fprintf(stderr, "[xinsp-fe] backend healthy (port %d up)\n", c.port);
                    // First healthy of a fresh (never-safe) start: reflect it too.
                    if (st.state != "healthy") {
                        st.state = "healthy"; st.reason.clear();
                        publish_status(c, st);
                    }
                }
                // Recovery: once this instance has been healthy continuously for
                // respawn_reset_ms, forget prior failures (the line recovered).
                if (healthy_since == 0) healthy_since = steady_ms();
                resp.note_healthy(steady_ms() - healthy_since, c.respawn_reset_ms);
                // Serve-time wedge: the heartbeat counter must keep advancing
                // while serving. If it stalls while the port still accepts, the
                // serving loop is wedged (a connect probe can't see this).
                if (c.heartbeat_stale_ms > 0) {
                    long long hb = read_heartbeat(c.heartbeat_file);
                    int64_t now = steady_ms();
                    if (!hb_armed) { hb_armed = true; hb_last_val = hb; hb_last_change_ms = now; }
                    else if (hb != hb_last_val) { hb_last_val = hb; hb_last_change_ms = now; }
                    else if (now - hb_last_change_ms > c.heartbeat_stale_ms) {
                        std::fprintf(stderr, "[xinsp-fe] backend heartbeat stale for >%d ms "
                                     "(serving loop wedged); killing for respawn\n",
                                     c.heartbeat_stale_ms);
                        death_reason = xi::BeExitReason::PortUnresponsive;
                        kill_confirmed = force_kill_be(sp.pi.hProcess, &exit_code);
                        break;
                    }
                }
                // Mirror the BE's canonical health into fe_status. The BE writes
                // its top-level state to health_file on each health_changed (a
                // lifecycle-rate write); we read that small file and republish the
                // status ONLY when it actually changes, so the FE write stays
                // lifecycle-rate too. This does NOT open a WS client — the single
                // client slot stays free for the HMI/extension.
                {
                    xi::BeHealth bh;
                    if (xi::parse_be_health(read_health_mirror(c.health_file), bh)) {
                        if (st.set_be_health(bh.state, bh.since_ms, bh.last_reason))
                            publish_status(c, st);
                    }
                }
            } else if (++probe_fails >= c.probe_fail_max) {
                std::fprintf(stderr, "[xinsp-fe] backend unresponsive (%d failed probes); "
                             "killing for respawn\n", probe_fails);
                death_reason = xi::BeExitReason::PortUnresponsive;
                kill_confirmed = force_kill_be(sp.pi.hProcess, &exit_code);
                break;
            }
        }

        bool intended = g_stop.load();
        CloseHandle(sp.pi.hThread);
        CloseHandle(sp.pi.hProcess);
        if (sp.log != INVALID_HANDLE_VALUE) CloseHandle(sp.log);

        if (intended) break;   // FE shutdown path handled after the loop

        // H1: the forced kill did NOT confirm the BE dead — it is STILL RUNNING on
        // this port + .xinsp_work dir. Respawning now would put a second live
        // backend on the same port/state → double-serve + shared-state corruption
        // (or BE#2's bind fails and latches the line falsely "down" while the zombie
        // keeps serving). Refuse: stop supervising and fall through to FE shutdown,
        // whose job-object close (KILL_ON_JOB_CLOSE) reaps the abandoned backend. A
        // process manager can then restart the FE clean.
        if (!kill_confirmed) {
            std::fprintf(stderr, "[xinsp-fe] backend did not terminate within the kill "
                         "budget — REFUSING to respawn (a second backend on port %d would "
                         "double-serve). Exiting so the job object reaps the abandoned "
                         "backend; restart the FE.\n", c.port);
            st.state  = "down";
            st.reason = "backend unkillable — FE exiting to reap it via job close";
            publish_status(c, st);
            rc = 1;
            break;
        }

        // ---- BE died unexpectedly: record forensics + respawn ----
        // ("Going safe" on a BE crash is now a comms plugin's own sidecar
        // process, not the FE — the FE just supervises: classify, log, respawn.)
        xi::BeExitEvent ev;
        ev.reason     = death_reason;
        ev.backend_rc = (int)exit_code;
        ev.ts_ms      = now_ms();
        xi::enrich_from_crash_report(c.be_log, ev);

        // ---- G2.2 per-plugin crash attribution + auto-quarantine ----
        // If the crash report attributed this death to a plugin (culprit cross-
        // checked against the faulting module), feed the per-plugin budget. When a
        // plugin TRIPS its budget we quarantine it (write G1's .xi_certify.json) AND
        // forgive the whole-backend consecutive counter — the offender is disabled
        // on the next respawn, so the supervisor's lever graduates from "latch the
        // whole line safe" to "drop the one bad plugin, keep the line up"
        // (Invariant §20.2). A death NOT attributable to a plugin (script/core
        // crash) flows through the normal whole-BE cap unchanged.
        bool quarantined_this_death = false;
        if (!ev.culprit_plugin.empty() && plugin_faults.note_fault(ev.culprit_plugin, ev.ts_ms)) {
            bool wrote = quarantine_plugin(ev.culprit_folder, ev.culprit_dll);
            quarantined_this_death = true;
            std::fprintf(stderr, "[xinsp-fe] plugin '%s' attributed %d crash(es) -> "
                         "QUARANTINED%s; respawning with it disabled (line stays up)\n",
                         ev.culprit_plugin.c_str(), plugin_faults.count(ev.culprit_plugin),
                         wrote ? "" : " (cache write failed — gate may not apply)");
            st.reason = "PluginQuarantined:" + ev.culprit_plugin;
            // Forgive the whole-line cap: the next BE comes up without this plugin,
            // so its prior deaths must not count toward latching the entire line.
            resp.reset();
        }

        // ---- respawn unless too many CONSECUTIVE failures (xi::RespawnTracker) ----
        // Counts deaths without a sustained-healthy recovery — so a recurring
        // fault latches safe whether its deaths are fast or slow. (A death that
        // just quarantined its culprit reset the counter above, so note_death here
        // starts that plugin's removal from a clean slate.)
        bool cap_hit = resp.note_death(c.respawn_max);
        // Append the structured crash record now that we know the consecutive
        // count and whether this death tripped the cap.
        history.record(ev, resp.consecutive, cap_hit);
        st.state = "down";
        // The dead BE's last health mirror is now stale — a gone process is not
        // "running". Drop it; the respawned BE rewrites boot->running and the next
        // healthy poll re-reads it. (fe_status.state="down" is the live truth here.)
        st.has_be_health = false; st.be_state.clear(); st.be_last_reason.clear();
        // Preserve the "PluginQuarantined:<id>" reason set above so the status
        // channel tells the operator the line is recovering by disabling a plugin,
        // not just that the BE died.
        if (!quarantined_this_death) st.reason = xi::to_string(death_reason);
        st.consecutive = resp.consecutive; st.backend_pid = 0;
        st.set_event(ev); publish_status(c, st);
        if (cap_hit) {
            std::fprintf(stderr, "[xinsp-fe] respawn limit (%d consecutive failures without "
                         "%ds sustained-healthy) exceeded - staying down; manual restart required\n",
                         c.respawn_max, c.respawn_reset_ms / 1000);
            st.latched = true; st.reason = "RespawnLimitExceeded";
            publish_status(c, st);
            rc = 2;
            break;
        }
        std::fprintf(stderr, "[xinsp-fe] respawning backend (failure %d/%d) after %dms\n",
                     resp.consecutive, c.respawn_max, c.respawn_backoff_ms);
        Sleep((DWORD)c.respawn_backoff_ms);
    }

    // Shutdown: closing the job kills the BE (KILL_ON_JOB_CLOSE).
    std::fprintf(stderr, "[xinsp-fe] supervisor stopping\n");
    {
        // Preserve a latched reason (e.g. RespawnLimitExceeded) so the final
        // snapshot still says WHY the line is down; only a clean stop overwrites it.
        st.state = "stopped"; st.backend_pid = 0;
        if (!st.latched) st.reason = "SupervisorShutdown";
        publish_status(c, st);
    }
    if (job) CloseHandle(job);
    WSACleanup();
    return rc;
}
#endif // _WIN32

static void print_help() {
    std::printf(
        "xinsp-fe - xInsp2 frontend supervisor\n"
        "\n"
        "Spawns + monitors xinsp-backend.exe; on backend death it records crash\n"
        "forensics and respawns it (rate-limited). Driving the production line to a\n"
        "safe state is a comms plugin's own sidecar process, not the FE.\n"
        "\n"
        "Usage: xinsp-fe [options]\n"
        "  --backend=PATH       xinsp-backend.exe (default: auto-discover)\n"
        "  --port=N             backend WS port (default 7823)\n"
        "  --project=DIR        project the backend should open + run headlessly\n"
        "  --script=PATH        script to compile (default: project.json's)\n"
        "  --autostart-fps=N    continuous fps for the backend (default 10; 0=off)\n"
        "  --plugins-dir=DIR    extra plugin folder (repeatable)\n"
        "  --be-log=PATH        where to capture the backend's stdout/stderr\n"
        "  --boot-timeout-ms=N  boot-hang budget: respawn if the backend never\n"
        "                       reaches 'ready' in N ms (default 60000; 0=off)\n"
        "  --heartbeat-stale-ms=N  serve-time wedge budget: respawn if the backend\n"
        "                       heartbeat stalls N ms while listening (default 15000; 0=off)\n"
        "  --health-file=PATH   the backend mirrors its canonical health here; the FE\n"
        "                       surfaces it in fe_status.be_health without a WS client\n"
        "                       (default: <be-log>.health)\n"
        "  --no-health-file     disable the BE health mirror\n"
        "  --watchdog-ms=N      per-inspect budget armed in the backend: catch a\n"
        "                       dispatch worker wedged in a plugin (the heartbeat\n"
        "                       can't) and respawn (default 60000; 0=off)\n"
        "  --be-arg=ARG         extra arg appended to the backend command (repeatable)\n"
        "  --working-copy       backend edits a <project>/.xinsp_work scratch;\n"
        "                       a crash respawn resumes it (settings survive)\n"
        "  --crash-history=PATH JSONL appended with one record per backend death\n"
        "                       (default: <be-log dir>/crash-history.jsonl)\n"
        "  --no-crash-history   disable the crash-history JSONL\n"
        "  --preserve-dumps     copy each referenced minidump (+ .json) somewhere\n"
        "                       stable so a temp-dir cleanup can't lose it\n"
        "  --preserve-dir=DIR   where --preserve-dumps copies to\n"
        "                       (default: <be-log dir>/crash-dumps)\n"
        "  --status-file=PATH   JSON status snapshot the FE rewrites on every\n"
        "                       transition (the UI reads the line's TRUE state)\n"
        "                       (default: <be-log dir>/fe-status.json)\n"
        "  --no-status-file     disable the status file\n"
        "  --config=PATH        fe.json config (CLI flags override it)\n"
        "  --help, -h           this help\n");
}

int main(int argc, char** argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        print_help();
        return 0;
    }
    FeConfig c = load_config(argc, argv);
#ifdef _WIN32
    return run_supervisor(c);
#else
    // TODO(linux): implement the supervisor with posix_spawn/fork+execv,
    // prctl(PR_SET_PDEATHSIG) for the kill-on-parent-death guarantee, a
    // ::connect probe (POSIX sockets), and SIGTERM/SIGINT handling. The
    // The crash-event model + crash-report parsing above are already portable.
    std::fprintf(stderr, "[xinsp-fe] not implemented on this platform yet "
                 "(backend=%s)\n", c.backend_exe.c_str());
    return 3;
#endif
}
