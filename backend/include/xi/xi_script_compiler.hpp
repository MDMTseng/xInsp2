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

} // namespace detail

inline CompileResult compile(const CompileRequest& req) {
    CompileResult r;
    std::filesystem::create_directories(req.output_dir);

    std::string vcvars = req.vcvars_path;
    if (vcvars.empty()) vcvars = detail::auto_find_vcvars();
    if (vcvars.empty()) {
        r.build_log = "vcvars64.bat not found — install VS Build Tools "
                      "or pass an explicit vcvars_path.";
        return r;
    }

    // Derive output names.
    std::filesystem::path src(req.source_path);
    std::string stem = src.stem().string();
    std::filesystem::path out_dll = std::filesystem::path(req.output_dir) / (stem + ".dll");
    std::filesystem::path log_path = std::filesystem::path(req.output_dir) / (stem + ".log");
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
    cmd += " /Fo\"" + req.output_dir + "\\\\\"";
    cmd += " /Fe\"" + out_dll.string() + "\"";
    cmd += " \"" + req.source_path + "\"";
    // Additional source files
    for (auto& s : req.extra_sources) {
        cmd += " \"" + s + "\"";
    }
    cmd += " /link /IMPLIB:\"" + (std::filesystem::path(req.output_dir) / (stem + ".lib")).string() + "\"";
    // Link against pre-built cjson.lib (needed by xi_record.hpp / xi_plugin_handle.hpp)
    auto cjson_lib = std::filesystem::path(req.include_dir).parent_path() / "build" / "Release" / "cjson.lib";
    if (std::filesystem::exists(cjson_lib)) {
        cmd += " \"" + cjson_lib.string() + "\"";
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
