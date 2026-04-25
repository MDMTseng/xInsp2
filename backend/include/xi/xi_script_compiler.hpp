#pragma once
//
// xi_script_compiler.hpp — thin wrapper around MSVC cl.exe that compiles
// a user C++ file into a DLL linked against the xInsp2 core headers.
//
// The backend exe knows its own path at startup and derives:
//   - include_dir — where xi/xi.hpp lives
//   - vendor_dir  — where stb_image_write.h lives (not needed by scripts
//                    in v1 but kept on the include path for future ops)
//
// Compilation happens by invoking `cmd /C` with vcvars64.bat + cl.exe.
// This requires VS Build Tools installed but no special project setup.
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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace xi::script {

struct CompileRequest {
    std::string source_path;               // primary .cpp (used for DLL name)
    std::vector<std::string> extra_sources; // additional .cpp files
    std::vector<std::string> include_dirs;  // extra include dirs
    std::string output_dir;
    std::string include_dir;    // backend/include (always added)
    std::string vcvars_path;
    // Optional accelerator install roots — when set, the script DLL is
    // compiled with the matching XINSP2_HAS_* define so xi::ops dispatch
    // gets the same backend as the main process. Empty = no acceleration
    // for that path (script falls back to portable C++).
    std::string opencv_dir;        // OpenCV install root with include/ and lib/
    std::string turbojpeg_root;    // libjpeg-turbo install root
    std::string ipp_root;          // Intel IPP install root (image ops only)
};

struct CompileResult {
    bool        ok = false;
    std::string dll_path;
    std::string build_log;
};

namespace detail {

// Search a few known VS install roots for vcvars64.bat. Returns empty
// string if not found.
inline std::string auto_find_vcvars() {
    const char* roots[] = {
        R"(C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat)",
        R"(C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat)",
        R"(C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat)",
        R"(C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat)",
        R"(C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat)",
        R"(C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat)",
    };
    for (const char* r : roots) {
        if (std::filesystem::exists(r)) return r;
    }
    return {};
}

inline std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Probe well-known install locations for accelerator libraries. Each
// probe checks for a sentinel header to confirm the layout. Empty
// string means "not found — script will fall back to portable C++".
inline std::string probe_opencv_dir() {
    if (const char* env = std::getenv("OpenCV_DIR")) {
        // OpenCV_DIR conventionally points at <root>/x64/<vc>/lib —
        // walk up to the actual install root.
        std::filesystem::path p(env);
        for (int i = 0; i < 3; ++i) {
            if (p.has_parent_path()) p = p.parent_path();
        }
        if (std::filesystem::exists(p / "include" / "opencv2" / "core.hpp")) return p.string();
    }
    const char* roots[] = {
        R"(C:\opencv\opencv\build)",
        R"(C:\opencv\build)",
    };
    for (const char* r : roots) {
        if (std::filesystem::exists(std::filesystem::path(r) / "include" / "opencv2" / "core.hpp"))
            return r;
    }
    return {};
}

inline std::string probe_turbojpeg_root() {
    if (const char* env = std::getenv("TURBOJPEG_ROOT")) {
        if (std::filesystem::exists(std::filesystem::path(env) / "include" / "turbojpeg.h"))
            return env;
    }
    const char* roots[] = {
        R"(C:\libjpeg-turbo64)",
        R"(C:\libjpeg-turbo)",
    };
    for (const char* r : roots) {
        if (std::filesystem::exists(std::filesystem::path(r) / "include" / "turbojpeg.h"))
            return r;
    }
    return {};
}

inline std::string probe_ipp_root() {
    if (const char* env = std::getenv("IPP_ROOT")) {
        if (std::filesystem::exists(std::filesystem::path(env) / "include" / "ippi.h"))
            return env;
    }
    std::error_code ec;
    for (auto& dir : std::filesystem::directory_iterator(R"(C:\Intel\ipp)", ec)) {
        if (std::filesystem::exists(dir.path() / "include" / "ippi.h")) return dir.path().string();
    }
    return {};
}

} // namespace detail

// Reject paths containing shell metacharacters to prevent command injection.
// These characters are illegal in Windows filenames anyway (except & and %).
inline bool is_safe_path(const std::string& p) {
    for (char c : p) {
        if (c == '&' || c == '|' || c == '>' || c == '<' ||
            c == '^' || c == '%' || c == '!' || c == '`') {
            return false;
        }
    }
    return true;
}

inline CompileResult compile(const CompileRequest& req) {
    CompileResult r;

    // Validate all paths against command injection
    auto check = [&](const std::string& p, const char* name) -> bool {
        if (!is_safe_path(p)) {
            r.build_log = std::string("rejected unsafe ") + name + ": " + p;
            return false;
        }
        return true;
    };
    if (!check(req.source_path, "source_path")) return r;
    if (!check(req.output_dir, "output_dir")) return r;
    if (!check(req.include_dir, "include_dir")) return r;
    for (auto& s : req.extra_sources) { if (!check(s, "extra_source")) return r; }
    for (auto& d : req.include_dirs) { if (!check(d, "include_dir")) return r; }

    std::filesystem::create_directories(req.output_dir);

    std::string vcvars = req.vcvars_path;
    if (vcvars.empty()) vcvars = detail::auto_find_vcvars();
    if (vcvars.empty()) {
        r.build_log = "vcvars64.bat not found — install VS Build Tools "
                      "or pass an explicit vcvars_path.";
        return r;
    }

    // Derive output names — versioned to avoid DLL file lock conflicts.
    // FreeLibrary doesn't always release the file handle immediately on
    // Windows, so we compile to a new filename each time.
    static std::atomic<int> s_version{0};
    std::filesystem::path src(req.source_path);
    std::string stem = src.stem().string();
    int ver = s_version++;
    std::string versioned_stem = stem + "_v" + std::to_string(ver);
    std::filesystem::path out_dll = std::filesystem::path(req.output_dir) / (versioned_stem + ".dll");
    std::filesystem::path log_path = std::filesystem::path(req.output_dir) / (versioned_stem + ".log");
    // Remove an existing dll so LoadLibrary can't grab a stale one on error.
    std::error_code ec;
    std::filesystem::remove(out_dll, ec);

    // Build the cl.exe command line.
    //
    //  /std:c++20   — match backend
    //  /LD          — build a DLL
    //  /EHsc        — standard exception semantics
    //  /MD          — multithreaded DLL runtime (matches stb/xi_core)
    //  /O2          — optimize
    //  /I<dir>      — xi headers
    //  /FI<hdr>     — force-include xi_script_support.hpp so user files
    //                 get the thunks for free
    //  /Fo...       — intermediate obj dir
    //  /Fe...       — output dll path
    //  /link /IMPLIB:NUL  (cl auto-generates an import lib; redirect to
    //                 intermediates dir to keep output_dir tidy)
    //
    std::string cmd;
    cmd += "cmd /C \"";
    cmd += "\"" + vcvars + "\"";
    cmd += " >nul 2>nul && ";
    cmd += "cl.exe /nologo /std:c++20 /LD /EHsc /MD /O2 /utf-8 /W3";
    cmd += " /I\"" + req.include_dir + "\"";
    // Also include the vendor dir (cJSON, stb, etc.) — sibling of include/
    auto vendor_dir = std::filesystem::path(req.include_dir).parent_path() / "vendor";
    if (std::filesystem::exists(vendor_dir)) {
        cmd += " /I\"" + vendor_dir.string() + "\"";
    }
    // Extra include dirs from project config
    for (auto& d : req.include_dirs) {
        cmd += " /I\"" + d + "\"";
    }
    cmd += " /FIxi/xi_script_support.hpp";
    // Accelerator includes — defines and -I; libs added at /link below.
    if (!req.opencv_dir.empty()) {
        cmd += " /D XINSP2_HAS_OPENCV=1";
        cmd += " /I\"" + req.opencv_dir + "\\include\"";
    }
    if (!req.turbojpeg_root.empty()) {
        cmd += " /D XINSP2_HAS_TURBOJPEG=1";
        cmd += " /I\"" + req.turbojpeg_root + "\\include\"";
    }
    if (!req.ipp_root.empty()) {
        cmd += " /D XINSP2_HAS_IPP=1";
        cmd += " /I\"" + req.ipp_root + "\\include\"";
    }
    cmd += " /Fo\"" + req.output_dir + "\\\\\"";
    cmd += " /Fe\"" + out_dll.string() + "\"";
    cmd += " \"" + req.source_path + "\"";
    // Additional source files
    for (auto& s : req.extra_sources) {
        cmd += " \"" + s + "\"";
    }
    cmd += " /link /IMPLIB:\"" + (std::filesystem::path(req.output_dir) / (versioned_stem + ".lib")).string() + "\"";
    // Link against pre-built cjson.lib (needed by xi_record.hpp / xi_plugin_handle.hpp)
    auto cjson_lib = std::filesystem::path(req.include_dir).parent_path() / "build" / "Release" / "cjson.lib";
    if (std::filesystem::exists(cjson_lib)) {
        cmd += " \"" + cjson_lib.string() + "\"";
    }
    // Accelerator import libs — match the /D defines added above.
    if (!req.opencv_dir.empty()) {
        // Pre-built OpenCV ships opencv_world<ver>.lib at x64/vc16/lib.
        std::error_code ec_lib;
        for (auto& vc : { "vc16", "vc17", "vc14" }) {
            auto libdir = std::filesystem::path(req.opencv_dir) / "x64" / vc / "lib";
            if (!std::filesystem::exists(libdir, ec_lib)) continue;
            for (auto& f : std::filesystem::directory_iterator(libdir, ec_lib)) {
                auto n = f.path().filename().string();
                if (n.rfind("opencv_world", 0) == 0 && n.size() > 4 &&
                    n.substr(n.size() - 4) == ".lib" &&
                    n.find('d') != n.size() - 5 /*skip *_d.lib debug*/) {
                    cmd += " \"" + f.path().string() + "\"";
                    break;
                }
            }
            break;
        }
    }
    if (!req.turbojpeg_root.empty()) {
        auto tj = std::filesystem::path(req.turbojpeg_root) / "lib" / "turbojpeg.lib";
        if (std::filesystem::exists(tj)) cmd += " \"" + tj.string() + "\"";
    }
    if (!req.ipp_root.empty()) {
        for (auto& n : { "ippcore.lib", "ippi.lib", "ippcv.lib", "ippcc.lib" }) {
            auto p = std::filesystem::path(req.ipp_root) / "lib" / n;
            if (std::filesystem::exists(p)) cmd += " \"" + p.string() + "\"";
        }
    }
    cmd += " > \"" + log_path.string() + "\" 2>&1";
    cmd += "\"";

    int rc = std::system(cmd.c_str());
    r.build_log = detail::read_file(log_path.string());
    // Sanitize build log: MSVC on non-English Windows may produce CP950/GBK
    // bytes that are not valid UTF-8. Replace any non-ASCII non-UTF8 byte
    // with '?' so the WS text frame stays legal.
    for (auto& c : r.build_log) {
        if (static_cast<unsigned char>(c) >= 0x80) c = '?';
    }

    if (rc == 0 && std::filesystem::exists(out_dll)) {
        r.ok = true;
        r.dll_path = out_dll.string();
    }
    return r;
}

} // namespace xi::script
