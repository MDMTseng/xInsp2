#pragma once
//
// xi_cli_args.hpp — command-line / environment argument parsing for the backend
// entrypoint. Pure helpers (argc/argv + env only, no backend state), extracted
// from service_main.cpp so main()'s flag handling lives in one place. Each
// returns a parsed value with its documented default; main() wires them to the
// globals. namespace xi::cli to keep the generic names (parse_port, has_flag)
// from colliding at global scope.
//
#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xi { namespace cli {

inline std::string get_exe_dir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::filesystem::path p(std::string(buf, n));
    return p.parent_path().string();
}

inline int parse_port(int argc, char** argv) {
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
inline std::string parse_host(int argc, char** argv) {
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
inline int parse_watchdog_ms(int argc, char** argv) {
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
inline std::string parse_auth_secret(int argc, char** argv) {
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
inline std::string parse_str_flag(int argc, char** argv, const char* flag) {
    std::string eq = std::string(flag) + "=";
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind(eq, 0) == 0) return std::string(a.substr(eq.size()));
        if (a == flag && i + 1 < argc) return argv[i + 1];
    }
    return {};
}

// Presence check for a bare flag (e.g. --hang-before-ready).
inline bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == flag) return true;
    return false;
}

// --autostart-fps=<N>  (default 0 = don't auto-start continuous mode; just
// open+compile and wait for a client / triggers). N < 0 = autostart in
// TRIGGER-ONLY mode (continuous on, lanes spawned, no synthetic timer tick —
// the project's sources drive everything).
inline int parse_autostart_fps(int argc, char** argv) {
    std::string v = parse_str_flag(argc, argv, "--autostart-fps");
    if (v.empty()) return 0;
    try { return std::stoi(v); } catch (...) { return 0; }
}

// Repeatable: --plugins-dir=/some/path  (or --plugins-dir /some/path).
// Also reads XINSP2_EXTRA_PLUGIN_DIRS, semicolon- or path-separator-delimited.
inline std::vector<std::string> parse_extra_plugin_dirs(int argc, char** argv) {
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

}} // namespace xi::cli
