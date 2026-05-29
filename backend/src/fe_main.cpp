//
// fe_main.cpp — xinsp-fe.exe, the frontend supervisor.
//
// Process isolation was removed (2026-05): a hard plugin crash takes the whole
// backend (BE) compute core down. The FE is the replacement safety net. It:
//   1. spawns xinsp-backend.exe (with --project/--autostart-fps so the BE runs
//      a line headlessly — no WS client needed; see service_main.cpp autostart),
//   2. monitors it (process-exit + a shallow TCP probe of the WS port),
//   3. the instant the BE dies, drives the line to a safe state via a
//      SafeStateSink (PLC abstraction; logging stub today) and reads the BE's
//      crash report for forensics,
//   4. respawns the BE with the same rate-limit the VS Code extension uses
//      (5 / 60s, 1.5s backoff); if that cap is exceeded it stays safe and waits
//      for a human.
//
// Windows-only today: process spawn/wait, Job Object, console-ctrl, and the TCP
// probe are Win32. The SafeStateSink seam, config/crash-log parsing stay
// portable. TODO(linux) markers below; see docs/design/linux-port.md.
//
#include <xi/xi_safe_state.hpp>

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

#include <cJSON.h>

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
    std::string safe_state_type = "log";
    std::string be_log;            // where the BE's stdout/stderr is captured
    // Respawn policy — mirrors the VS Code extension (extension.ts).
    int         respawn_window_s   = 60;
    int         respawn_max        = 5;
    int         respawn_backoff_ms = 1500;
    // Liveness probe.
    int         probe_interval_ms  = 1000;
    int         probe_fail_max     = 5;   // consecutive failures (proc alive) -> unresponsive
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

// Walk up from the FE exe dir looking for xinsp-backend.exe (mirrors
// service_main.cpp's parent-walk for include/plugins dirs).
static std::string discover_backend_exe() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    fs::path dir = fs::path(std::string(buf, n)).parent_path();
#else
    fs::path dir = fs::current_path();
#endif
    const char* exe = "xinsp-backend"
#ifdef _WIN32
        ".exe"
#endif
        ;
    fs::path p = dir;
    for (int i = 0; i < 6; ++i) {
        if (fs::exists(p / exe)) return (p / exe).string();
        if (!p.has_parent_path() || p.parent_path() == p) break;
        p = p.parent_path();
    }
    return (dir / exe).string();  // best guess: sibling of the FE
}

static FeConfig load_config(int argc, char** argv) {
    FeConfig c;

    // Optional fe.json (CLI flags override it). Keys mirror the flags.
    std::string cfg_path = arg_value(argc, argv, "--config");
    if (cfg_path.empty() && fs::exists("fe.json")) cfg_path = "fe.json";
    if (!cfg_path.empty()) {
        std::ifstream f(cfg_path);
        std::stringstream ss; ss << f.rdbuf();
        if (cJSON* root = cJSON_Parse(ss.str().c_str())) {
            auto str = [&](const char* k, std::string& dst) {
                cJSON* v = cJSON_GetObjectItem(root, k);
                if (cJSON_IsString(v) && v->valuestring) dst = v->valuestring;
            };
            auto num = [&](const char* k, int& dst) {
                cJSON* v = cJSON_GetObjectItem(root, k);
                if (cJSON_IsNumber(v)) dst = (int)v->valuedouble;
            };
            str("backend", c.backend_exe);
            num("port", c.port);
            str("project", c.project);
            str("script", c.script);
            num("autostart_fps", c.autostart_fps);
            str("safe_state", c.safe_state_type);
            str("be_log", c.be_log);
            num("respawn_max", c.respawn_max);
            num("respawn_window_s", c.respawn_window_s);
            num("respawn_backoff_ms", c.respawn_backoff_ms);
            if (cJSON* pd = cJSON_GetObjectItem(root, "plugins_dirs"); cJSON_IsArray(pd)) {
                cJSON* it = nullptr;
                cJSON_ArrayForEach(it, pd)
                    if (cJSON_IsString(it) && it->valuestring) c.plugins_dirs.push_back(it->valuestring);
            }
            cJSON_Delete(root);
        }
    }

    // CLI overrides.
    if (auto v = arg_value(argc, argv, "--backend");        !v.empty()) c.backend_exe = v;
    if (auto v = arg_value(argc, argv, "--port");           !v.empty()) try { c.port = std::stoi(v); } catch (...) {}
    if (auto v = arg_value(argc, argv, "--project");        !v.empty()) c.project = v;
    if (auto v = arg_value(argc, argv, "--script");         !v.empty()) c.script = v;
    if (auto v = arg_value(argc, argv, "--autostart-fps");  !v.empty()) try { c.autostart_fps = std::stoi(v); } catch (...) {}
    if (auto v = arg_value(argc, argv, "--safe-state");     !v.empty()) c.safe_state_type = v;
    if (auto v = arg_value(argc, argv, "--be-log");         !v.empty()) c.be_log = v;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--plugins-dir=", 0) == 0) c.plugins_dirs.push_back(a.substr(14));
        else if (a == "--plugins-dir" && i + 1 < argc) c.plugins_dirs.push_back(argv[++i]);
    }

    if (c.backend_exe.empty()) c.backend_exe = discover_backend_exe();
    if (c.be_log.empty())      c.be_log = (fs::temp_directory_path() / "xinsp2-fe-be.log").string();
    return c;
}

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ----------------------------------------------------------------------------
// Crash report — parse the BE's own log for the absolute minidump path it
// printed, then read the sibling .json. This is the proven approach from
// plugin_crash_forensics/driver.py: do NOT guess %TEMP%/xinsp2/crashdumps,
// because a sandboxed/per-tool TEMP isn't inherited by the BE we spawned.
// ----------------------------------------------------------------------------
static void enrich_from_crash_report(const std::string& be_log, xi::SafeStateEvent& ev) {
    std::ifstream f(be_log);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    std::string text = ss.str();

    // BE prints: "... — minidump: C:\...\xinsp-backend-<pid>-<ts>.dmp"
    std::regex re(R"(minidump:\s*(.+?\.dmp))");
    std::string dmp;
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
        dmp = (*it)[1].str();   // keep the last match
    if (dmp.empty()) return;
    // Trim trailing whitespace/CR.
    while (!dmp.empty() && (dmp.back() == '\r' || dmp.back() == '\n' || dmp.back() == ' '))
        dmp.pop_back();

    fs::path json_path = fs::path(dmp).replace_extension(".json");
    ev.report_path = json_path.string();
    std::ifstream jf(json_path);
    if (!jf) return;
    std::stringstream js; js << jf.rdbuf();
    cJSON* root = cJSON_Parse(js.str().c_str());
    if (!root) return;
    if (cJSON* exc = cJSON_GetObjectItem(root, "exception")) {
        if (cJSON* nm = cJSON_GetObjectItem(exc, "name"); cJSON_IsString(nm))
            ev.exception_name = nm->valuestring;
        if (cJSON* md = cJSON_GetObjectItem(exc, "module"); cJSON_IsString(md))
            ev.faulting_module = md->valuestring;
    }
    if (cJSON* ctx = cJSON_GetObjectItem(root, "context")) {
        if (cJSON* ph = cJSON_GetObjectItem(ctx, "last_phase"); cJSON_IsString(ph))
            ev.last_phase = ph->valuestring;
    }
    // If context didn't name a phase (crash on an unmanaged thread), fall back
    // to the most informative thread breadcrumb in threads[].
    if (ev.last_phase.empty()) {
        if (cJSON* th = cJSON_GetObjectItem(root, "threads"); cJSON_IsArray(th)) {
            cJSON* t = nullptr;
            cJSON_ArrayForEach(t, th) {
                cJSON* ph = cJSON_GetObjectItem(t, "last_phase");
                if (cJSON_IsString(ph) && ph->valuestring && ph->valuestring[0]) {
                    ev.last_phase = ph->valuestring;
                    if (std::string(ph->valuestring) == "inspect") break;  // prefer inspect
                }
            }
        }
    }
    cJSON_Delete(root);
}

#ifdef _WIN32
// ----------------------------------------------------------------------------
// Win32 supervisor
// ----------------------------------------------------------------------------
static std::atomic<bool> g_stop{false};   // set by console-ctrl handler

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

static int run_supervisor(const FeConfig& c) {
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    // Sink logs to stderr itself; pass null so it doesn't double-print.
    auto sink = xi::make_safe_state_sink(c.safe_state_type, nullptr);
    std::fprintf(stderr, "[xinsp-fe] supervisor up: backend=%s port=%d project=%s fps=%d sink=%s\n",
                 c.backend_exe.c_str(), c.port, c.project.c_str(), c.autostart_fps, sink->name());
    std::fprintf(stderr, "[xinsp-fe] BE log -> %s\n", c.be_log.c_str());

    // Job object: kill the BE if the FE dies, so it can never orphan.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl{};
        jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof(jl));
    }

    std::vector<int64_t> respawns;   // ms timestamps, sliding window
    bool in_safe_state = false;
    int  rc = 0;

    while (!g_stop.load()) {
        Spawned sp = spawn_backend(c, job);
        if (!sp.ok) {
            xi::SafeStateEvent ev; ev.reason = xi::SafeStateReason::BackendExit;
            ev.ts_ms = now_ms();
            sink->enter_safe_state(ev);
            rc = 1;
            break;
        }
        std::fprintf(stderr, "[xinsp-fe] backend up pid=%lu\n", sp.pi.dwProcessId);

        // ---- monitor this instance ----
        int  probe_fails = 0;
        bool healthy_announced = false;
        DWORD exit_code = 0;
        xi::SafeStateReason death_reason = xi::SafeStateReason::BackendExit;
        for (;;) {
            DWORD w = WaitForSingleObject(sp.pi.hProcess, (DWORD)c.probe_interval_ms);
            if (w == WAIT_OBJECT_0) {                       // BE exited
                GetExitCodeProcess(sp.pi.hProcess, &exit_code);
                death_reason = xi::SafeStateReason::BackendExit;
                break;
            }
            if (g_stop.load()) break;
            // Still alive — probe the port.
            if (port_open(c.port)) {
                probe_fails = 0;
                if (in_safe_state && !healthy_announced) {
                    sink->clear_safe_state();
                    in_safe_state = false;
                    healthy_announced = true;
                    std::fprintf(stderr, "[xinsp-fe] backend healthy (port %d up)\n", c.port);
                }
            } else if (++probe_fails >= c.probe_fail_max) {
                std::fprintf(stderr, "[xinsp-fe] backend unresponsive (%d failed probes); "
                             "killing for respawn\n", probe_fails);
                death_reason = xi::SafeStateReason::PortUnresponsive;
                TerminateProcess(sp.pi.hProcess, 1);
                WaitForSingleObject(sp.pi.hProcess, 5000);
                GetExitCodeProcess(sp.pi.hProcess, &exit_code);
                break;
            }
        }

        bool intended = g_stop.load();
        CloseHandle(sp.pi.hThread);
        CloseHandle(sp.pi.hProcess);
        if (sp.log != INVALID_HANDLE_VALUE) CloseHandle(sp.log);

        if (intended) break;   // FE shutdown path handled after the loop

        // ---- BE died unexpectedly: go safe + record forensics ----
        xi::SafeStateEvent ev;
        ev.reason     = death_reason;
        ev.backend_rc = (int)exit_code;
        ev.ts_ms      = now_ms();
        enrich_from_crash_report(c.be_log, ev);
        sink->enter_safe_state(ev);
        in_safe_state = true;

        // ---- rate-limited respawn ----
        int64_t t = now_ms();
        int64_t window = (int64_t)c.respawn_window_s * 1000;
        respawns.erase(std::remove_if(respawns.begin(), respawns.end(),
                       [&](int64_t ts){ return t - ts > window; }), respawns.end());
        if ((int)respawns.size() >= c.respawn_max) {
            xi::SafeStateEvent stuck;
            stuck.reason = xi::SafeStateReason::RespawnLimitExceeded;
            stuck.ts_ms  = now_ms();
            stuck.exception_name  = ev.exception_name;
            stuck.faulting_module = ev.faulting_module;
            stuck.report_path     = ev.report_path;
            sink->enter_safe_state(stuck);
            std::fprintf(stderr, "[xinsp-fe] respawn limit (%d in %ds) exceeded — staying in "
                         "safe state; manual restart required\n", c.respawn_max, c.respawn_window_s);
            rc = 2;
            break;
        }
        respawns.push_back(t);
        std::fprintf(stderr, "[xinsp-fe] respawning backend (%d/%d in window) after %dms\n",
                     (int)respawns.size(), c.respawn_max, c.respawn_backoff_ms);
        Sleep((DWORD)c.respawn_backoff_ms);
    }

    // Shutdown: closing the job kills the BE (KILL_ON_JOB_CLOSE).
    std::fprintf(stderr, "[xinsp-fe] supervisor stopping\n");
    {
        xi::SafeStateEvent ev; ev.reason = xi::SafeStateReason::SupervisorShutdown;
        ev.ts_ms = now_ms();
        sink->enter_safe_state(ev);
    }
    if (job) CloseHandle(job);
    WSACleanup();
    return rc;
}
#endif // _WIN32

static void print_help() {
    std::printf(
        "xinsp-fe — xInsp2 frontend supervisor\n"
        "\n"
        "Spawns + monitors xinsp-backend.exe, drives the line to a safe state on\n"
        "backend death, and respawns it (rate-limited).\n"
        "\n"
        "Usage: xinsp-fe [options]\n"
        "  --backend=PATH       xinsp-backend.exe (default: auto-discover)\n"
        "  --port=N             backend WS port (default 7823)\n"
        "  --project=DIR        project the backend should open + run headlessly\n"
        "  --script=PATH        script to compile (default: project.json's)\n"
        "  --autostart-fps=N    continuous fps for the backend (default 10; 0=off)\n"
        "  --plugins-dir=DIR    extra plugin folder (repeatable)\n"
        "  --safe-state=TYPE    safe-state sink (default 'log')\n"
        "  --be-log=PATH        where to capture the backend's stdout/stderr\n"
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
    // SafeStateSink seam + crash-report parsing above are already portable.
    std::fprintf(stderr, "[xinsp-fe] not implemented on this platform yet "
                 "(backend=%s)\n", c.backend_exe.c_str());
    return 3;
#endif
}
