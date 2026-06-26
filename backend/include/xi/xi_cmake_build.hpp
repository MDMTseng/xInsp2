#pragma once
//
// xi_cmake_build.hpp — host-side cmake invocation for "build": "cmake" plugins.
//
// A plugin with `"build": "cmake"` owns its own build (its CMakeLists handles
// external libs / CUDA); the backend shells out to cmake to configure + build
// it rather than cl.exe-compiling the source itself. These are the pure process
// + filesystem mechanics of that: scan a source tree's newest mtime (the
// rebuild change-gate), capture a child process's output, and configure+build
// one plugin. The PluginManager keeps the stateful orchestration
// (rebuild_cmake_plugins: which plugins, unload/reload, restore instances).
//
// Extracted from xi_plugin_manager.hpp — no PluginManager state, so a leaf the
// manager calls into.
//
// TODO(linux): run_cmd_capture has the _WIN32 _popen path; the popen() branch is
// stubbed pending the Linux port (see linux-port.md).
//
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace xi {
namespace cmake_build {

// Newest write-time (ticks) over a cmake plugin's source tree — the rebuild
// change-gate input. Counts source + build-script files, skips the build/ tree
// (its artifacts aren't "source"). 0 if nothing found.
inline uint64_t newest_source_mtime(const std::string& dir) {
    uint64_t newest = 0;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return 0;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory()) {
            if (it->path().filename() == "build") it.disable_recursion_pending();
            continue;
        }
        auto ext = it->path().extension().string();
        auto fn  = it->path().filename().string();
        bool src = ext == ".c"  || ext == ".cpp" || ext == ".cc"  || ext == ".cxx" ||
                   ext == ".cu" || ext == ".cuh" || ext == ".h"   || ext == ".hpp" ||
                   ext == ".hxx"|| fn == "CMakeLists.txt" || ext == ".cmake";
        if (!src) continue;
        auto wt = std::filesystem::last_write_time(it->path(), ec);
        if (!ec) newest = std::max(newest, (uint64_t)wt.time_since_epoch().count());
    }
    return newest;
}

// Run a command, capturing combined stdout+stderr into *log. Returns the
// process exit code (or -1 if it couldn't be spawned).
inline int run_cmd_capture(const std::string& cmd, std::string& log) {
#ifdef _WIN32
    // _popen routes through cmd.exe; the extra outer quotes keep a quoted
    // exe path + quoted args parsing correctly.
    std::string full = "\"" + cmd + " 2>&1\"";
    FILE* pipe = _popen(full.c_str(), "r");
    if (!pipe) { log += "[failed to spawn: " + cmd + "]\n"; return -1; }
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) log += buf;
    int rc = _pclose(pipe);
    return rc;
#else
    // TODO(linux): popen(cmd + " 2>&1") — same shape, no outer-quote wrap.
    (void)cmd; log += "[cmake build unsupported on this platform]\n"; return -1;
#endif
}

// Configure (first time) + build one cmake plugin. Appends logs. 0 = success.
// `opencv_dir` is the host's top-level OpenCV pack dir (may be empty).
inline int build_cmake_plugin(const std::string& cmake_exe, const std::string& src_dir,
                              const std::string& config, const std::string& xinsp_root,
                              const std::string& opencv_dir, std::string& log) {
    auto build_dir = (std::filesystem::path(src_dir) / "build").string();
    // The command goes through cmd.exe (_popen). `cmake_exe` and `config` come
    // from the WS client, so reject shell metacharacters (and a `"` that could
    // break out of quoting) before they reach the shell — command-injection
    // guard, mirrors the cl.exe driver's is_safe_path. Paths are quoted below;
    // Windows paths can't contain `"` and `&`/`^` are inert inside quotes.
    auto unsafe = [](const std::string& s) {
        return s.find_first_of("&|<>^%!\"`\n\r") != std::string::npos;
    };
    if (unsafe(cmake_exe) || unsafe(config)) {
        log += "[refused: cmake_exe/config contains shell metacharacters]\n";
        return -1;
    }
    auto q = [](const std::string& s) { return "\"" + s + "\""; };
    if (!std::filesystem::exists(std::filesystem::path(build_dir) / "CMakeCache.txt")) {
        std::string cfg = q(cmake_exe) + " -S " + q(src_dir) + " -B " + q(build_dir) +
                          " -A x64 -DXINSP2_ROOT=" + q(xinsp_root);
        // OpenCV: pass the resolved `x64/vcNN/lib` SUBDIR (the level whose
        // OpenCVConfig.cmake actually resolves), NOT the top-level pack dir —
        // forcing the top dir makes OpenCV's runtime auto-detect set
        // OpenCV_FOUND=FALSE. opencv_dir is the host's top pack dir; derive the
        // vc subdir so a NON-standard OpenCV location also works. If none
        // matches, omit it and let xinsp2_plugin.cmake's own (hard-coded
        // C:/opencv) probe try.
        if (!opencv_dir.empty()) {
            for (const char* vc : {"vc17", "vc16", "vc15", "vc14"}) {
                auto lib = std::filesystem::path(opencv_dir) / "x64" / vc / "lib";
                if (std::filesystem::exists(lib / "OpenCVConfig.cmake")) {
                    cfg += " -DOpenCV_DIR=" + q(lib.string());
                    break;
                }
            }
        }
        log += "[configure] " + cfg + "\n";
        int rc = run_cmd_capture(cfg, log);
        if (rc != 0) return rc;
    }
    std::string bld = q(cmake_exe) + " --build " + q(build_dir) + " --config " + q(config);
    log += "[build] " + bld + "\n";
    return run_cmd_capture(bld, log);
}

} // namespace cmake_build
} // namespace xi
