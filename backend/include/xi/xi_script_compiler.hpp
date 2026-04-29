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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace xi::script {

// What kind of DLL we're building. Affects:
//   - which header gets force-included (script vs plugin support)
//   - debug vs optimized codegen / PDB emission
//   - whether the produced DLL is intended to be exported as a standalone
//     plugin (Export mode = Release + PDB stripped + cert pre-baked)
enum class CompileMode {
    Script,         // user inspection.cpp — Release, /O2, force-include script support
    PluginDev,      // project-local plugin during development — Debug, /Zi /Od
    PluginExport,   // export-ready build — Release, /O2, /Zi (PDB beside DLL for crash blame)
};

struct CompileRequest {
    std::string source_path;               // primary .cpp (used for DLL name)
    std::vector<std::string> extra_sources; // additional .cpp files
    std::vector<std::string> include_dirs;  // extra include dirs
    std::string output_dir;
    std::string include_dir;    // backend/include (always added)
    std::string vcvars_path;
    CompileMode mode = CompileMode::Script;
    // OpenCV install root — REQUIRED. Plugins/scripts include
    // <opencv2/opencv.hpp> directly via xi.hpp / xi_plugin_support.hpp,
    // so the compile step needs the include + lib paths wired in.
    std::string opencv_dir;
    // Optional accelerator roots for the JPEG-encode dispatch in
    // xi_jpeg.hpp; not used for image operators (those are cv::).
    std::string turbojpeg_root;    // libjpeg-turbo install root
    std::string ipp_root;          // Intel IPP install root
};

// One diagnostic line parsed out of cl.exe / link.exe output.
// `file` is whatever cl.exe printed (may be relative); upstream code
// is expected to resolve to an absolute workspace path.
struct Diagnostic {
    std::string file;
    int         line     = 0;   // 1-based; 0 if unknown (e.g. linker errors)
    int         col      = 0;   // 1-based; 0 if unknown
    std::string severity;       // "error" | "warning" | "note"
    std::string code;           // "C2065", "LNK2019", ...
    std::string message;
};

struct CompileResult {
    bool                    ok = false;
    std::string             dll_path;
    std::string             build_log;
    std::vector<Diagnostic> diagnostics;
};

// Parse cl.exe / link.exe output into structured diagnostics. The format
// is one of:
//   foo.cpp(42,15): error C2065: 'x': undeclared identifier
//   foo.cpp(42): warning C4996: ...
//   foo.obj : error LNK2019: unresolved external symbol ...
// Lines that don't match any of these patterns are skipped.
inline std::vector<Diagnostic> parse_diagnostics(const std::string& log) {
    std::vector<Diagnostic> out;
    size_t pos = 0;
    while (pos < log.size()) {
        size_t eol = log.find('\n', pos);
        std::string line = log.substr(pos, (eol == std::string::npos ? log.size() : eol) - pos);
        pos = (eol == std::string::npos) ? log.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Find ": error " / ": warning " / ": fatal error " / ": note "
        // *after* a possible (line,col) suffix on the file portion.
        // `has_code` distinguishes "<sev> CXXXX: msg" (error/warning/fatal,
        // which always carry a code) from "note: msg" (no code).
        struct SevMatch { const char* tag; const char* sev; bool has_code; };
        const SevMatch sevs[] = {
            { ": fatal error ", "error",   true  },
            { ": error ",       "error",   true  },
            { ": warning ",     "warning", true  },
            { ": note: ",       "note",    false },
        };
        size_t sev_at = std::string::npos;
        const char* sev_name = nullptr;
        size_t sev_len = 0;
        bool has_code = true;
        for (auto& s : sevs) {
            size_t hit = line.find(s.tag);
            if (hit != std::string::npos && (sev_at == std::string::npos || hit < sev_at)) {
                sev_at = hit;
                sev_name = s.sev;
                sev_len = std::strlen(s.tag);
                has_code = s.has_code;
            }
        }
        if (sev_at == std::string::npos) continue;

        Diagnostic d;
        d.severity = sev_name;

        // Left of severity: "<file>" or "<file>(line)" or "<file>(line,col)".
        std::string left = line.substr(0, sev_at);
        // Trim trailing space (e.g. "foo.obj ")
        while (!left.empty() && (left.back() == ' ' || left.back() == '\t')) left.pop_back();
        if (!left.empty() && left.back() == ')') {
            size_t op = left.rfind('(');
            if (op != std::string::npos) {
                std::string loc = left.substr(op + 1, left.size() - op - 2);
                d.file = left.substr(0, op);
                size_t comma = loc.find(',');
                try {
                    if (comma == std::string::npos) {
                        d.line = std::stoi(loc);
                    } else {
                        d.line = std::stoi(loc.substr(0, comma));
                        d.col  = std::stoi(loc.substr(comma + 1));
                    }
                } catch (...) { /* leave 0 */ }
            } else {
                d.file = left;
            }
        } else {
            d.file = left;
        }

        // Right of severity: "<code>: <message>" for error/warning,
        // or just "<message>" for notes.
        std::string right = line.substr(sev_at + sev_len);
        if (has_code) {
            size_t colon = right.find(':');
            if (colon != std::string::npos) {
                d.code    = right.substr(0, colon);
                size_t mstart = colon + 1;
                while (mstart < right.size() && right[mstart] == ' ') ++mstart;
                d.message = right.substr(mstart);
            } else {
                d.message = right;
            }
        } else {
            d.message = right;
        }
        out.push_back(std::move(d));
    }
    return out;
}

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
    // Force English diagnostics regardless of system locale. Without
    // this, cl.exe / link.exe emit messages in whatever language the
    // OS UI is set to (Traditional Chinese / Japanese / German / ...)
    // which (a) confuses agents and tools that expect English keywords
    // for parsing, and (b) round-trips through the local code page on
    // the way to the SDK, producing mojibake. /utf-8 is already passed
    // below for source + execution charset, but it doesn't override
    // the diagnostic language. VSLANG=1033 == LCID for en-US.
    cmd += "set VSLANG=1033 && ";
    // Mode-dependent codegen flags:
    //   Script        → /O2 (Release) — main inspection loop wants speed
    //   PluginDev     → /Od /Zi /RTC1 — debugger-friendly, fast iteration
    //   PluginExport  → /O2 /Zi — Release perf + keep PDB for crash blame
    const char* opt_flags =
        (req.mode == CompileMode::PluginDev) ? "/Od /Zi /RTC1"
      : (req.mode == CompileMode::PluginExport) ? "/O2 /Zi"
      : "/O2";
    cmd += "cl.exe /nologo /std:c++20 /LD /EHsc /MD ";
    cmd += opt_flags;
    cmd += " /utf-8 /W3";
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
    // Force-include the right convenience header for the kind of DLL.
    // Script gets the inspect_entry plumbing; plugin gets the C ABI
    // export macros so the user only writes a plain class.
    if (req.mode == CompileMode::Script) {
        cmd += " /FIxi/xi_script_support.hpp";
    } else {
        cmd += " /FIxi/xi_plugin_support.hpp";
    }
    // OpenCV is mandatory: xi.hpp / xi_plugin_support.hpp pull in
    // <opencv2/opencv.hpp> for cv:: image operators.
    if (req.opencv_dir.empty()) {
        CompileResult r;
        r.ok = false;
        r.build_log = "OpenCV not configured (set OpenCV_DIR or install to a default location); xInsp2 requires OpenCV for image operators";
        return r;
    }
    cmd += " /I\"" + req.opencv_dir + "\\include\"";
    if (!req.turbojpeg_root.empty()) {
        cmd += " /D XINSP2_HAS_TURBOJPEG=1";
        cmd += " /I\"" + req.turbojpeg_root + "\\include\"";
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
    // Sanitize build log for the WS text frame, which must be UTF-8.
    // cl.exe / link.exe emit diagnostics in the system code page (CP950
    // on zh-TW Windows, CP932 on ja-JP, CP1252 on en-US, etc.). Even
    // with /utf-8 passed for source + execution charset, message text
    // localized for non-English locales lands in the local ACP. The
    // result is mojibake on the wire and a useless friction-log entry
    // for AI agents trying to act on the diagnostic.
    //
    // Strategy: if the log is already valid UTF-8 (English locale, or
    // VSLANG=1033 took effect), leave it alone. Otherwise convert from
    // the active code page → wide → UTF-8. Only if BOTH steps fail do
    // we fall back to the old strip-non-ASCII-to-? heuristic.
#ifdef _WIN32
    auto is_valid_utf8 = [](const std::string& s) {
        size_t i = 0, n = s.size();
        while (i < n) {
            unsigned char c = (unsigned char)s[i];
            int extra;
            if      (c < 0x80) extra = 0;
            else if ((c >> 5) == 0x06) extra = 1;
            else if ((c >> 4) == 0x0E) extra = 2;
            else if ((c >> 3) == 0x1E) extra = 3;
            else return false;
            if (i + (size_t)extra >= n) return false;
            for (int k = 1; k <= extra; ++k)
                if ((((unsigned char)s[i + k]) >> 6) != 0x02) return false;
            i += (size_t)extra + 1;
        }
        return true;
    };
    if (!is_valid_utf8(r.build_log) && !r.build_log.empty()) {
        bool converted = false;
        int n_in = (int)r.build_log.size();
        int wlen = MultiByteToWideChar(CP_ACP, 0, r.build_log.data(), n_in, nullptr, 0);
        if (wlen > 0) {
            std::wstring w((size_t)wlen, L'\0');
            if (MultiByteToWideChar(CP_ACP, 0, r.build_log.data(), n_in, w.data(), wlen) == wlen) {
                int u8len = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
                if (u8len > 0) {
                    std::string u8((size_t)u8len, '\0');
                    if (WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, u8.data(), u8len, nullptr, nullptr) == u8len) {
                        r.build_log = std::move(u8);
                        converted = true;
                    }
                }
            }
        }
        if (!converted) {
            for (auto& c : r.build_log)
                if (static_cast<unsigned char>(c) >= 0x80) c = '?';
        }
    }
#else
    // TODO(linux): cl.exe path doesn't apply; build log will already be
    // UTF-8 from gcc/clang in typical setups. Leave as-is.
#endif

    if (rc == 0 && std::filesystem::exists(out_dll)) {
        r.ok = true;
        r.dll_path = out_dll.string();
    }
    r.diagnostics = parse_diagnostics(r.build_log);
    return r;
}

} // namespace xi::script
