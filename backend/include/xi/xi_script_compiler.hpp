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
#else
  #include <sys/wait.h>   // WIFEXITED / WEXITSTATUS for popen'd compile
  #include <unistd.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "xi_proc.hpp"   // round-3 #6: the ONE bounded win32 spawn

namespace xi::script {

// Reap per-PID script-build dirs left by DEAD backends under `base` (J1). Each
// per-PID dir holds a ~200 MB PCH, so without reaping they pile up across every run
// and fill the disk — the parallel-QA harness alone spawns dozens. A `<pid>` subdir
// is reapable when no live process owns that pid. Called at startup so each new
// backend collects its dead predecessors; best-effort (skip anything unclassifiable
// or in-use). Never removes a live backend's dir (concurrent starts see each other
// as alive).
inline void reap_stale_build_dirs(const std::filesystem::path& base) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(base, ec)) return;
    for (const auto& e : fs::directory_iterator(base, ec)) {
        if (ec) break;
        if (!e.is_directory(ec)) continue;
        unsigned long pid = 0;
        try { pid = std::stoul(e.path().filename().string()); }
        catch (...) { continue; }   // not a <pid> dir — leave it
        bool alive = false;
#ifdef _WIN32
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid)) {
            alive = (WaitForSingleObject(h, 0) == WAIT_TIMEOUT);
            CloseHandle(h);
        }
#endif
        if (!alive) fs::remove_all(e.path(), ec);
    }
}

// What kind of DLL we're building. Affects:
//   - which header gets force-included (script vs plugin support)
//   - debug vs optimized codegen / PDB emission
//   - whether the produced DLL is intended to be exported as a standalone
//     plugin (Export mode = Release + PDB stripped + cert pre-baked)
enum class CompileMode {
    Script,         // user inspect.cpp — Release, /O2, force-include script support
    PluginDev,      // project-local plugin during development — Debug, /Zi /Od
    PluginExport,   // export-ready build — Release, /O2, /Zi (PDB beside DLL for crash blame)
};

struct CompileRequest {
    std::string source_path;               // primary .cpp (used for DLL name)
    std::vector<std::string> extra_sources; // additional .cpp files
    std::vector<std::string> include_dirs;  // extra include dirs (project.json "include_dirs")
    std::vector<std::string> link_libs;     // extra import libs to link (project.json "link_libs")
    std::string output_dir;
    std::string include_dir;    // backend/include (always added)
    std::string vcvars_path;
    CompileMode mode = CompileMode::Script;
    // Fast dev compile: /Od instead of /O2 for the Script path — much faster to
    // COMPILE (the dev-loop bottleneck), at the cost of slower inspect runtime.
    // Default off (optimized) so autostart/production stay /O2; the interactive
    // compile_and_load handler turns it on unless the client asks to optimize.
    bool fast = false;
    // OpenMP thread cap (opt-in via project.json "openmp_max_threads"):
    //   0  → OFF (no /openmp) — default; production/autostart compiles unchanged.
    //   N>0→ ON, capped to N threads (compile adds /openmp + /D XI_OMP_MAX_THREADS=N;
    //        xi_script_support.hpp calls omp_set_num_threads(N) at DLL load).
    //   -1 → ON, uncapped (all cores) — /openmp, no cap define.
    // One knob = on/off AND the oversubscription ceiling. Links vcomp140.dll
    // (in System32, no extra deploy). See docs/guides/write-a-script.md.
    int openmp_max_threads = 0;
    // Blessed-concurrency guard (adoption map item 11): a raw OpenMP pragma
    // written directly in a SCRIPT's own source is rejected at compile time and
    // the author is steered to xi::parallel_for / xi::async (which install the
    // SEH translator + image-pool owner on every worker). false = reject (default,
    // Script mode only); true = an explicit project.json "allow_raw_omp" opt-out
    // for a power user who accepts the worker-thread safety rules. Ignored for
    // plugin modes (plugins own their own threads + crash posture).
    bool allow_raw_omp = false;
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

// True if `s` is well-formed UTF-8. Portable (no Win32). Used to decide whether
// compiler diagnostics need transcoding before they go on the (UTF-8) wire.
inline bool is_valid_utf8(const std::string& s) {
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
}

// Guarantee `s` is valid UTF-8 for the WS text frame. cl.exe / link.exe localize
// diagnostics to the OS UI language and emit them in the local code page (CP950
// zh-TW, CP932 ja-JP, CP1252 en-US, ...) even with /utf-8 + VSLANG=1033 (the
// latter no-ops when the en-US compiler language pack isn't installed). Raw
// CP950 bytes on a UTF-8 wire are mojibake — useless to a UI or an AI agent.
// Strategy: already-valid UTF-8 is returned unchanged; otherwise convert via the
// OEM code page (what cl writes to a redirected handle — equals ACP on zh-TW but
// differs on en-US, so OEM not ACP). Last resort: strip non-ASCII to '?' so the
// result is ALWAYS valid UTF-8. This is the invariant test_diagnostics locks in.
inline std::string ensure_utf8(std::string s) {
#ifdef _WIN32
    if (s.empty() || is_valid_utf8(s)) return s;
    bool converted = false;
    int n_in = (int)s.size();
    const UINT diag_cp = GetOEMCP();
    int wlen = MultiByteToWideChar(diag_cp, 0, s.data(), n_in, nullptr, 0);
    if (wlen > 0) {
        std::wstring w((size_t)wlen, L'\0');
        if (MultiByteToWideChar(diag_cp, 0, s.data(), n_in, w.data(), wlen) == wlen) {
            int u8len = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
            if (u8len > 0) {
                std::string u8((size_t)u8len, '\0');
                if (WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, u8.data(), u8len, nullptr, nullptr) == u8len) {
                    s = std::move(u8);
                    converted = true;
                }
            }
        }
    }
    if (!converted)
        for (auto& c : s)
            if (static_cast<unsigned char>(c) >= 0x80) c = '?';
#else
    // POSIX: gcc/clang emit UTF-8, so valid diagnostics pass through untouched.
    // The invariant is that the RESULT is always valid UTF-8 (the WS frame is
    // UTF-8) — there is no code-page transcode to do, but if a diagnostic ever
    // carries non-UTF-8 bytes (a mislocalized external toolchain, a stray byte),
    // replace the non-ASCII bytes so nothing invalid reaches the wire. The ASCII
    // skeleton (error codes, quotes, paths) survives intact.
    if (!s.empty() && !is_valid_utf8(s))
        for (auto& c : s)
            if (static_cast<unsigned char>(c) >= 0x80) c = '?';
#endif
    return s;
}

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

// Parse gcc/clang diagnostics (the POSIX compile driver). The format is:
//   foo.cpp:42:15: error: 'x' was not declared in this scope
//   foo.cpp:42: warning: ...            (no column)
//   foo.cpp:42:15: note: ...
//   /usr/bin/ld: foo.o: undefined reference to 'bar'   (linker — no line/col)
// The severity marker is " <sev>: " after the "file:line:col" prefix. Caret /
// "In file included from" / "candidate:" continuation lines carry no marker and
// are skipped, matching the MSVC parser's line-oriented behaviour.
inline std::vector<Diagnostic> parse_diagnostics_gcc(const std::string& log) {
    std::vector<Diagnostic> out;
    auto is_num = [](const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) if (c < '0' || c > '9') return false;
        return true;
    };
    struct SevMatch { const char* tag; const char* sev; };
    const SevMatch sevs[] = {
        { ": fatal error: ", "error"   },
        { ": error: ",       "error"   },
        { ": warning: ",     "warning" },
        { ": note: ",        "note"    },
    };
    size_t pos = 0;
    while (pos < log.size()) {
        size_t eol = log.find('\n', pos);
        std::string line = log.substr(pos, (eol == std::string::npos ? log.size() : eol) - pos);
        pos = (eol == std::string::npos) ? log.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        size_t sev_at = std::string::npos;
        const char* sev_name = nullptr;
        size_t sev_len = 0;
        for (auto& s : sevs) {
            size_t hit = line.find(s.tag);
            if (hit != std::string::npos && (sev_at == std::string::npos || hit < sev_at)) {
                sev_at = hit; sev_name = s.sev; sev_len = std::strlen(s.tag);
            }
        }
        if (sev_at == std::string::npos) continue;

        Diagnostic d;
        d.severity = sev_name;
        d.message  = line.substr(sev_at + sev_len);

        // Left of the marker: "<file>", "<file>:<line>", or "<file>:<line>:<col>".
        std::string left = line.substr(0, sev_at);
        size_t c1 = left.rfind(':');
        if (c1 != std::string::npos && is_num(left.substr(c1 + 1))) {
            std::string last = left.substr(c1 + 1);
            size_t c2 = (c1 == 0) ? std::string::npos : left.rfind(':', c1 - 1);
            std::string mid = (c2 == std::string::npos) ? std::string() : left.substr(c2 + 1, c1 - c2 - 1);
            try {
                if (c2 != std::string::npos && is_num(mid)) {
                    d.file = left.substr(0, c2);
                    d.line = std::stoi(mid);
                    d.col  = std::stoi(last);
                } else {
                    d.file = left.substr(0, c1);
                    d.line = std::stoi(last);
                }
            } catch (...) { d.file = left; }
        } else {
            d.file = left;   // linker / driver error with no line info
        }
        out.push_back(std::move(d));
    }
    return out;
}

// VAR(name, expr) expands to `auto name = ...`, so the SAME name twice in one
// scope is a plain C++ redefinition — cl reports a terse "'name': redefinition"
// (C2374/C2086/...) pointing at the macro, which is easy to misread. This maps
// such an error back to the VAR()/VAR_RAW() call sites and appends a clear,
// actionable hint (only when the symbol really is VAR'd 2+ times — zero false
// positives, since the compiler already proved it's a redefinition).
inline void augment_var_redefinitions(std::vector<Diagnostic>& diags,
                                      const std::string& source_path) {
    auto is_redef = [](const std::string& code) {
        return code == "C2374" || code == "C2086" || code == "C2371" || code == "C2365";
    };
    bool any = false;
    for (auto& d : diags) if (d.severity == "error" && is_redef(d.code)) { any = true; break; }
    if (!any) return;

    std::ifstream f(source_path);
    if (!f) return;
    std::vector<std::string> lines;
    for (std::string ln; std::getline(f, ln); ) lines.push_back(ln);

    auto is_ident = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    auto var_sites = [&](const std::string& sym) {
        std::vector<int> hits;
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& L = lines[i];
            for (const char* macro : { "VAR(", "VAR_RAW(" }) {
                size_t mlen = std::strlen(macro), p = 0;
                while ((p = L.find(macro, p)) != std::string::npos) {
                    size_t a = p + mlen;
                    while (a < L.size() && (L[a] == ' ' || L[a] == '\t')) ++a;
                    if (L.compare(a, sym.size(), sym) == 0) {
                        size_t e = a + sym.size();
                        if (e >= L.size() || !is_ident(L[e])) hits.push_back((int)i + 1);
                    }
                    p = a;
                }
            }
        }
        return hits;
    };

    for (auto& d : diags) {
        if (d.severity != "error" || !is_redef(d.code)) continue;
        size_t q1 = d.message.find('\'');
        if (q1 == std::string::npos) continue;
        size_t q2 = d.message.find('\'', q1 + 1);
        if (q2 == std::string::npos) continue;
        std::string sym = d.message.substr(q1 + 1, q2 - q1 - 1);
        auto sites = var_sites(sym);
        if (sites.size() < 2) continue;
        std::string hint = "  >> xinsp2: duplicate VAR(" + sym + ") at line";
        hint += sites.size() > 1 ? "s " : " ";
        for (size_t i = 0; i < sites.size(); ++i) { if (i) hint += ", "; hint += std::to_string(sites[i]); }
        hint += " — VAR declares a variable, so the same name twice in one scope is a "
                "redefinition. Rename one, or use EMIT(" + sym + ") to surface an existing "
                "value without re-declaring.";
        d.message += "\n" + hint;
    }
}

// Blessed-concurrency guard (adoption map item 11 / concurrency review finding 4).
// Return the 1-based line numbers of RAW OpenMP directives (`#pragma omp ...`)
// found in a script's own source text.
//
// Why reject rather than translate: a hardware fault inside a raw `#pragma omp`
// region runs on a vcomp worker thread that has NO per-thread SEH translator, so
// it is not converted to a catchable xi::seh_exception — it terminates the whole
// backend — and pool images the region creates are tagged owner=0 and escape the
// per-script leak sweep. The blessed xi::parallel_for / xi::async wrappers install
// the translator + owner on every worker; nothing else guarantees it (the DLL-load
// warmup in xi_script_support.hpp is a best-effort floor, not airtight for nested /
// dynamic / grown teams). We can't simply drop /openmp to neutralize the raw form:
// those same wrappers are header-only and expand `#pragma omp parallel` INTO the
// script TU, so they REQUIRE /openmp to actually parallelize. The precise boundary
// is therefore a scan of the AUTHOR's own source (never the xi headers, which carry
// the blessed pragma) that steers a raw directive to the wrappers.
//
// Detection is line-based on preprocessor-directive shape: optional leading
// whitespace, '#', optional whitespace, "pragma", whitespace, "omp". That is
// exactly how a real `#pragma omp ...` must appear (directives are line-oriented),
// so it never matches "omp" inside a string/identifier on a code line. A trailing
// `//` line comment is stripped first so a commented-out example isn't flagged.
// (A raw-string literal spanning lines whose content mimics a directive is a known
// false positive — rare in inspect scripts, and the "allow_raw_omp" opt-out covers
// the author who genuinely needs it.)
inline std::vector<int> raw_omp_pragma_lines(const std::string& src) {
    std::vector<int> hits;
    size_t pos = 0;
    int lineno = 0;
    while (pos <= src.size()) {
        size_t eol = src.find('\n', pos);
        std::string line = src.substr(pos, (eol == std::string::npos ? src.size() : eol) - pos);
        pos = (eol == std::string::npos) ? src.size() + 1 : eol + 1;
        ++lineno;
        // Strip a // line comment (naive — fine for directive detection).
        size_t slashes = line.find("//");
        if (slashes != std::string::npos) line.resize(slashes);
        size_t i = 0;
        auto skip_ws = [&] { while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i; };
        skip_ws();
        if (i >= line.size() || line[i] != '#') continue;
        ++i; skip_ws();
        if (line.compare(i, 6, "pragma") != 0) continue;
        i += 6;
        if (i < line.size() && line[i] != ' ' && line[i] != '\t') continue;  // "pragmaX"
        skip_ws();
        if (line.compare(i, 3, "omp") != 0) continue;
        i += 3;
        if (i < line.size() && line[i] != ' ' && line[i] != '\t') continue;  // "ompX"
        hits.push_back(lineno);
    }
    return hits;
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

#ifdef _WIN32
// Capture the environment vcvars64.bat sets up, ONCE per vcvars path, as a
// CreateProcess environment block. Running vcvars64.bat costs ~1-2s and used to
// happen on EVERY compile — the dominant dev-loop latency. We run it once, snapshot
// `set`, and hand the block to cl.exe directly on later compiles. Returns nullptr
// if capture failed (caller falls back to the inline vcvars path). Thread-safe.
inline const std::vector<char>* vcvars_env_block(const std::string& vcvars) {
    static std::mutex mu;
    static std::map<std::string, std::vector<char>> cache;
    std::lock_guard<std::mutex> lk(mu);
    if (auto it = cache.find(vcvars); it != cache.end())
        return it->second.size() > 1 ? &it->second : nullptr;

    std::vector<char> block;
    std::string cmd = "cmd /C \"" + xi::proc::quote_arg(vcvars) + " >nul 2>nul && set\"";
    if (FILE* pipe = _popen(cmd.c_str(), "r")) {
        char line[32768];
        bool have_vslang = false;
        while (std::fgets(line, sizeof(line), pipe)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.find('=') == std::string::npos) continue;     // skip stray noise
            if (s.rfind("VSLANG=", 0) == 0) { have_vslang = true; s = "VSLANG=1033"; }
            block.insert(block.end(), s.begin(), s.end());
            block.push_back('\0');
        }
        _pclose(pipe);
        if (!have_vslang) {                                     // force English diags
            static const char kV[] = "VSLANG=1033";
            block.insert(block.end(), kV, kV + sizeof(kV) - 1);
            block.push_back('\0');
        }
        block.push_back('\0');                                   // double-null terminate
    }
    auto& slot = (cache[vcvars] = std::move(block));
    return slot.size() > 1 ? &slot : nullptr;
}

// Generous ceiling on a single cl.exe invocation. A wedged toolchain (mspdbsrv
// contention, an AV-locked link output, a hung vcvars child) must NOT freeze the
// poll thread — and thus ALL WS commands — indefinitely. On timeout we kill the
// child and treat it as a compile failure (mirrors the certify path's
// WaitForSingleObject-with-timeout + TerminateProcess in xi_certify.hpp).
static constexpr DWORD kCompileTimeoutMs = 300000;   // 5 min

// Run a command line under a custom environment block (nullptr = inherit the
// parent's); returns the process exit code, or -1 if it couldn't be launched,
// or -2 if it exceeded kCompileTimeoutMs (the caller reports a "toolchain
// timed out" diagnostic). (cl.exe is invoked via `cmd /C` so the env's PATH
// resolves cl + the redirection works.)
//
// Round-3 #6: delegates to xi::proc::spawn_bounded. Two gaps closed:
//   * the old copy killed only the immediate cmd.exe on timeout — orphaned
//     cl.exe/mspdbsrv kept the .dll/.pdb locked; the job object now reaps the
//     whole tree;
//   * both compile paths fell back to UNBOUNDED std::system when the vcvars
//     env capture failed — a wedged cl pinned the poll thread forever (the
//     exact class kCompileTimeoutMs was added to kill). The fallback is now a
//     bounded inherit-env spawn (the command line routes vcvars inline, so
//     inherit-env is the std::system-equivalent), env == nullptr here.
// Return-code semantics (0/exit-code, -1 spawn failure, -2 timeout) are
// unchanged for all callers.
inline int run_with_env(const std::string& cmdline, const std::vector<char>* env) {
    return xi::proc::spawn_bounded(cmdline, kCompileTimeoutMs, env, /*capture=*/nullptr);
}

// Build (once, cached by a flag+exe signature) a precompiled header for the
// OpenCV umbrella <opencv2/opencv.hpp> — by far the heaviest front-end parse in a
// script compile. The umbrella is force-included FIRST (the /Yu boundary) and the
// xi headers re-include it harmlessly (include-guarded). We deliberately PCH only
// OpenCV, NOT xi.hpp: xi.hpp pulls xi_io.hpp which references the mutable file-
// statics defined in xi_script_support.hpp, so PCH-ing through it would split that
// state across TUs and silently break xi::use()/triggers at runtime.
//
// Best-effort: returns {} on ANY failure (capture, compile error) so the caller
// falls back to a normal compile — PCH never breaks the build, only speeds it.
struct PchResult { std::string pch, obj; };
inline PchResult ensure_pch(const std::string& output_dir, const std::string& flags,
                            const char* through, const std::vector<char>* env,
                            const std::string& vcvars) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::path(output_dir) / ".pch";
    fs::create_directories(dir, ec);
    fs::path pch = dir / "umbrella.pch", obj = dir / "umbrella.obj",
             sigf = dir / "umbrella.sig", stub = dir / "umbrella.cpp",
             plog = dir / "umbrella.log";
    // Invalidate when the flags OR the backend binary change (a framework rebuild
    // may change the headers without changing the flags).
    std::string sig = std::string(through) + "|" + flags;
    char exe[MAX_PATH]; DWORD en = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (en) { auto t = fs::last_write_time(fs::path(std::string(exe, en)), ec);
              if (!ec) sig += "|" + std::to_string(t.time_since_epoch().count()); }
    {
        std::ifstream sf(sigf); std::stringstream s; s << sf.rdbuf();
        if (s.str() == sig && fs::exists(pch) && fs::exists(obj))
            return { pch.string(), obj.string() };
    }
    { std::ofstream o(stub); o << "#include <" << through << ">\n"; }
    const auto q = &xi::proc::quote_arg;   // round-3 W2 #7: the shared quoter
    std::string cmd = "cmd /C \"";
    if (!env) cmd += q(vcvars) + " >nul 2>nul && set VSLANG=1033 && ";
    cmd += "cl.exe " + flags + " /c /Yc" + q(through) + " /Fp" + q(pch.string())
         + " /Fo" + q(obj.string()) + " " + q(stub.string())
         + " > " + q(plog.string()) + " 2>&1\"";
    // Round-3 #6: no more std::system fallback — env == nullptr spawns bounded
    // with the parent env inherited (the cmd line routes vcvars inline).
    int rc = run_with_env(cmd, env);
    if (rc != 0 || !fs::exists(pch) || !fs::exists(obj)) return {};
    { std::ofstream o(sigf); o << sig; }
    return { pch.string(), obj.string() };
}
#endif // _WIN32

#ifndef _WIN32
// Run a shell command, capturing combined stdout+stderr into `out`. Returns the
// child's exit code, or -1 if it could not be spawned / exited abnormally.
inline int run_capture_posix(const std::string& cmd, std::string& out) {
    std::string full = cmd + " 2>&1";
    FILE* p = ::popen(full.c_str(), "r");
    if (!p) return -1;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    int rc = ::pclose(p);
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 128;
}

// Query pkg-config (e.g. "--cflags opencv4"); returns the trimmed output, or ""
// if pkg-config fails / the package is missing.
inline std::string pkgconfig(const std::string& args) {
    std::string out;
    if (run_capture_posix("pkg-config " + args, out) != 0) return {};
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}
#endif // !_WIN32

} // namespace detail

// Reject paths containing shell metacharacters to prevent command injection.
// These characters are illegal in Windows filenames anyway (except & and %).
// The double-quote is rejected too: the compile runs as `cmd /C "... cl ...
// \"<arg>\" ..."`, so an embedded `"` lets a value close its quoted arg and
// splice in `& <cmd>`. Quotes never appear in a legitimate path/lib name.
inline bool is_safe_path(const std::string& p) {
    for (char c : p) {
        if (c == '&' || c == '|' || c == '>' || c == '<' ||
            c == '^' || c == '%' || c == '!' || c == '`' ||
            c == '"') {
            return false;
        }
    }
    return true;
}

#ifndef _WIN32
// POSIX compile driver: g++/clang++ -fPIC -shared, the Linux/macOS replacement
// for the cl.exe path below. Produces a .so; OpenCV via pkg-config; the yyjson
// codec is compiled straight from its single vendored .c; force-includes mirror
// the cl.exe /FI order (OpenCV umbrella, then the mode's support header). Called
// by compile() AFTER the shared, portable validation + raw-OpenMP guard have run,
// so it assumes validated input and just builds. (The cl.exe path's _vN prune is
// a Windows disk-churn mitigation not replicated here — ELF has no DLL file lock.)
inline CompileResult compile_posix_build_(const CompileRequest& req) {
    namespace fs = std::filesystem;
    CompileResult r;

    static std::atomic<int> s_version{0};
    std::string stem = fs::path(req.source_path).stem().string();
    int ver = s_version++;
    std::string versioned_stem = stem + "_v" + std::to_string(ver);
    fs::path out_so   = fs::path(req.output_dir) / (versioned_stem + ".so");
    fs::path log_path = fs::path(req.output_dir) / (versioned_stem + ".log");
    std::error_code ec;
    fs::remove(out_so, ec);

    // OpenCV via pkg-config — the Linux analogue of the Windows opencv_dir probe.
    std::string ocv_cflags = detail::pkgconfig("--cflags opencv4");
    std::string ocv_libs   = detail::pkgconfig("--libs opencv4");
    if (ocv_libs.empty()) {
        r.build_log = "OpenCV not found (pkg-config opencv4 failed); install "
                      "libopencv-dev (or set PKG_CONFIG_PATH)";
        return r;
    }

    const char* cxx_env = std::getenv("CXX");
    std::string cxx = (cxx_env && *cxx_env) ? cxx_env : "g++";

    // Codegen per mode (closest flag equivalents to the cl.exe modes).
    //   -fnon-call-exceptions + -fno-gnu-unique: the fault→seh_exception unwind
    //   and hot-reload-unload contracts the backend build documents — a script/
    //   plugin MUST carry them too (the host catches its faults; the loader
    //   unloads it). -fvisibility=default keeps the XI_EXPORT thunks exported.
    std::string opt =
        (req.mode == CompileMode::PluginDev)          ? "-O0 -g"
      : (req.mode == CompileMode::Script && req.fast) ? "-O0 -g"
      :                                                 "-O2 -g";

    const auto q = &xi::proc::quote_arg;   // round-3 W2 #7: the shared quoter
    // -fpermissive: the script/plugin support headers declare the per-module
    // globals (g_use_host_api_ etc.) `extern` in xi_io/xi_use then DEFINE them
    // `static` in xi_script_support (each module needs its own copy). MSVC accepts
    // that extern→static narrowing natively; g++ rejects it unless -fpermissive
    // downgrades this specific linkage-conformance class to a warning. It does NOT
    // relax diagnostics for ordinary user errors (undeclared names, type errors),
    // so a real mistake in the author's inspect.cpp still fails the compile.
    std::string cmd = cxx + " -std=c++20 -fPIC -shared -fexceptions -fpermissive"
                            " -fnon-call-exceptions -fno-gnu-unique -fvisibility=default "
                    + opt;
    if (req.openmp_max_threads != 0) cmd += " -fopenmp";

    cmd += " -I" + q(req.include_dir);
    fs::path vendor_dir = fs::path(req.include_dir).parent_path() / "vendor";
    if (fs::exists(vendor_dir)) cmd += " -I" + q(vendor_dir.string());
    fs::path yyjson_inc = vendor_dir / "yyjson";
    if (fs::exists(yyjson_inc)) cmd += " -I" + q(yyjson_inc.string());
    for (auto& d : req.include_dirs) cmd += " -I" + q(d);
    cmd += " " + ocv_cflags;

    if (req.openmp_max_threads > 0)
        cmd += " -D XI_OMP_MAX_THREADS=" + std::to_string(req.openmp_max_threads);

    cmd += " -include opencv2/opencv.hpp";
    cmd += (req.mode == CompileMode::Script) ? " -include xi/xi_script_support.hpp"
                                             : " -include xi/xi_plugin_support.hpp";

    cmd += " -o " + q(out_so.string());
    cmd += " " + q(req.source_path);
    for (auto& s : req.extra_sources) cmd += " " + q(s);

    // yyjson codec: some SDK headers pull it in. It is C, so it CANNOT share this
    // command — the C++-only force-includes (-include opencv2/opencv.hpp) apply to
    // every input and would break a C file ("must be compiled as C++"). Compile it
    // to a cached object in its own C invocation (no force-includes), then link
    // that object in. Best-effort: if the object build fails the link just omits
    // it (a script that truly needs yyjson then fails at link with a clear error).
    fs::path yyjson_c = yyjson_inc / "yyjson.c";
    if (fs::exists(yyjson_c)) {
        fs::path yyjson_o = fs::path(req.output_dir) / "xi_yyjson_c.o";
        if (!fs::exists(yyjson_o, ec)) {
            std::string cc = cxx + " -x c -c -fPIC -O2 -fvisibility=default -I"
                           + q(yyjson_inc.string())
                           + " -o " + q(yyjson_o.string()) + " " + q(yyjson_c.string());
            std::string clog;
            detail::run_capture_posix(cc, clog);
        }
        if (fs::exists(yyjson_o, ec)) cmd += " " + q(yyjson_o.string());
    }

    cmd += " " + ocv_libs;
    if (req.openmp_max_threads != 0) cmd += " -fopenmp";
    for (auto& l : req.link_libs) cmd += " " + q(l);

    cmd += " > " + q(log_path.string()) + " 2>&1";

    int rc = std::system(cmd.c_str());
    r.build_log   = detail::read_file(log_path.string());   // gcc/clang emit UTF-8
    r.diagnostics = parse_diagnostics_gcc(r.build_log);
    if (rc == 0 && fs::exists(out_so)) {
        r.ok = true;
        r.dll_path = out_so.string();
    }
    return r;
}
#endif // !_WIN32

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
    // Link + toolchain inputs ALSO land on the cl.exe command line: link_libs
    // at the link step, and the toolchain roots into /I flags + the vcvars
    // prefix. They come from project.json just like the paths above, so they
    // need the same injection guard. (Root cause of the link_libs command
    // injection: these were omitted from the validation set.)
    for (auto& l : req.link_libs) { if (!check(l, "link_lib")) return r; }
    if (!check(req.opencv_dir,     "opencv_dir"))     return r;
    if (!check(req.turbojpeg_root, "turbojpeg_root")) return r;
    if (!check(req.ipp_root,       "ipp_root"))       return r;
    if (!check(req.vcvars_path,    "vcvars_path"))    return r;

    // Blessed-concurrency guard (adoption map item 11): reject a raw `#pragma omp`
    // in a SCRIPT's own source BEFORE cl.exe runs, routing the author to the
    // SEH-safe xi::parallel_for / xi::async wrappers. Script mode only (plugins own
    // their own threads); an explicit project.json "allow_raw_omp": true opts out.
    // The scan reads only the author's source (source_path + extra_sources), never
    // the xi headers — so xi_parallel.hpp's own blessed `#pragma omp` is untouched.
    if (req.mode == CompileMode::Script && !req.allow_raw_omp) {
        std::vector<Diagnostic> omp_diags;
        auto scan_one = [&](const std::string& path) {
            std::string text = detail::read_file(path);
            for (int ln : raw_omp_pragma_lines(text)) {
                Diagnostic d;
                d.file     = path;
                d.line     = ln;
                d.col      = 0;
                d.severity = "error";
                d.code     = "XI9001";
                d.message  =
                    "raw OpenMP pragma is not allowed in an inspection script. A hardware "
                    "fault inside a raw '#pragma omp' region runs on a worker thread with no "
                    "SEH translator, so it bypasses crash isolation and terminates the whole "
                    "backend, and pool images it creates leak (owner=0). Use "
                    "xi::parallel_for(n, [&](int i){ ... }) for a pixel/row loop, or "
                    "xi::async(...) for independent fan-out -- both install the SEH translator "
                    "and image-pool owner on every worker. See docs/guides/write-a-script.md "
                    "(Parallelism safety). To override at your own risk, set "
                    "\"allow_raw_omp\": true in project.json.";
                omp_diags.push_back(std::move(d));
            }
        };
        scan_one(req.source_path);
        for (auto& s : req.extra_sources) scan_one(s);
        if (!omp_diags.empty()) {
            std::string log =
                "xInsp2: raw OpenMP pragma rejected in script -- use xi::parallel_for / "
                "xi::async (see docs/guides/write-a-script.md):\n";
            for (auto& d : omp_diags)
                log += "  " + d.file + "(" + std::to_string(d.line) + ")\n";
            r.ok          = false;
            r.build_log   = std::move(log);
            r.diagnostics = std::move(omp_diags);
            return r;
        }
    }

    std::filesystem::create_directories(req.output_dir);

#ifndef _WIN32
    // POSIX: hand off to the g++/clang++ driver. The portable validation +
    // raw-OpenMP guard above have already run; everything below is cl.exe-only.
    return compile_posix_build_(req);
#endif

    std::string vcvars = req.vcvars_path;
    if (vcvars.empty()) vcvars = detail::auto_find_vcvars();
    // Re-check the FINAL vcvars value: the configured field was validated above,
    // but on the empty-path branch it's replaced by auto_find_vcvars() (derived
    // from env vars like VSINSTALLDIR), which then lands in the `cmd /C "..."`
    // string — so it needs the same injection guard.
    if (!check(vcvars, "vcvars")) return r;
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
    //  /EHa         — async exception semantics. REQUIRED, not cosmetic: the
    //                 host runs every plugin/script call under
    //                 _set_se_translator (xi_seh.hpp), which turns an SEH fault
    //                 (access violation, /0, …) into a C++ seh_exception that
    //                 the backend's try/catch absorbs. MSVC only emits the
    //                 unwind funclets that make that translation sound — and
    //                 that run local dtors as the exception propagates out —
    //                 under /EHa. Under /EHsc the catch in the /EHa host still
    //                 fires (backend survives), but RAII in the faulting
    //                 plugin/script frame is SKIPPED, leaking e.g. the
    //                 pool_image it held (1 ImagePool slot per absorbed crash).
    //                 Must match the backend EXE's /EHa (CMakeLists.txt) and the
    //                 "Requires /EHa" contract in docs/overview.md.
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
    // Dev-loop latency: running vcvars64.bat per compile cost ~1-2s. Capture its
    // environment once (detail::vcvars_env_block) and run cl.exe directly under it.
    // Only when that capture fails do we fall back to the inline vcvars path.
#ifdef _WIN32
    const std::vector<char>* cl_env = detail::vcvars_env_block(vcvars);
#else
    const std::vector<char>* cl_env = nullptr;
#endif
    const auto q = &xi::proc::quote_arg;   // round-3 W2 #7: the shared quoter
    std::string cmd;
    cmd += "cmd /C \"";
    if (!cl_env) {
        cmd += q(vcvars);
        cmd += " >nul 2>nul && ";
        // Force English diagnostics regardless of system locale (cl localizes to
        // the OS UI language → mojibake / unparseable on non-en hosts). VSLANG=1033
        // == en-US. When cl_env is used, VSLANG=1033 is baked into that block.
        cmd += "set VSLANG=1033 && ";
    }
    // Mode-dependent codegen flags:
    //   Script        → /O2 /Z7 — Release perf + symbolicated crash stacks.
    //                   The inspect script is where most crashes originate
    //                   (user logic calling plugins); without debug info a
    //                   minidump shows `inspect_vN.dll+0x1234` with no line
    //                   info. /Z7 embeds debug info in the .obj (no separate
    //                   compiler vcNNN.pdb to collide across parallel
    //                   compiles); the linker /DEBUG below then emits one
    //                   versioned program PDB matched to the DLL. /Z7 does
    //                   NOT disable /O2, so runtime perf is unchanged.
    //                   (Crash-diagnosability work, 2026-05.)
    //   PluginDev     → /Od /Zi /RTC1 — debugger-friendly, fast iteration
    //   PluginExport  → /O2 /Zi — Release perf + keep PDB for crash blame
    const char* opt_flags =
        (req.mode == CompileMode::PluginDev) ? "/Od /Zi /RTC1"
      : (req.mode == CompileMode::PluginExport) ? "/O2 /Zi"
      : (req.mode == CompileMode::Script && req.fast) ? "/Od /Z7"   // fast dev iterate
      : "/O2 /Z7";
    // OpenCV is mandatory: xi.hpp / xi_plugin_support.hpp pull in
    // <opencv2/opencv.hpp> for cv:: image operators.
    if (req.opencv_dir.empty()) {
        CompileResult rr;
        rr.ok = false;
        rr.build_log = "OpenCV not configured (set OpenCV_DIR or install to a default location); xInsp2 requires OpenCV for image operators";
        return rr;
    }
    // Shared front-end flags (codegen + ALL include/define flags) — assembled once
    // so the PCH is built with byte-identical flags to the consuming compile.
    std::string front = std::string("/nologo /std:c++20 /EHa /MD ") + opt_flags + " /utf-8 /W3";
    // OpenMP opt-in (project.json "openmp_max_threads" != 0). /openmp must live in
    // `front` so the PCH is built with byte-identical codegen flags. Legacy
    // /openmp → vcomp140.dll (System32). The numeric cap is injected as a macro
    // on the consume compile below (NOT in front — keeps the PCH from re-keying
    // per cap value). TODO(linux): emit -fopenmp for gcc/clang instead.
    if (req.openmp_max_threads != 0) front += " /openmp";
    front += " /I" + q(req.include_dir);
    // vendor dir (stb, etc.) — sibling of include/
    auto vendor_dir = std::filesystem::path(req.include_dir).parent_path() / "vendor";
    if (std::filesystem::exists(vendor_dir)) front += " /I" + q(vendor_dir.string());
    // yyjson lives in vendor/yyjson/ — xi_json.hpp (and a script's own direct
    // #include "yyjson.h") needs it on the include path.
    auto yyjson_inc = vendor_dir / "yyjson";
    if (std::filesystem::exists(yyjson_inc)) front += " /I" + q(yyjson_inc.string());
    for (auto& d : req.include_dirs) front += " /I" + q(d);   // project extra includes
    front += " /I" + q(req.opencv_dir + "\\include");
    if (!req.turbojpeg_root.empty()) {
        front += " /D XINSP2_HAS_TURBOJPEG=1";
        front += " /I" + q(req.turbojpeg_root + "\\include");
    }

    // Precompiled header for the OpenCV umbrella — the dominant parse cost. Best-
    // effort (ensure_pch returns {} on any failure → plain compile). Script only:
    // the plugin path force-includes a different support header.
    std::string pch_obj;
#ifdef _WIN32
    if (req.mode == CompileMode::Script) {
        auto p = detail::ensure_pch(req.output_dir, front, "opencv2/opencv.hpp", cl_env, vcvars);
        if (!p.pch.empty()) {
            // /FI the umbrella FIRST (the /Yu boundary); xi headers re-include it
            // harmlessly (include-guarded). Then the script support header.
            front += " /FIopencv2/opencv.hpp /Yu" + q("opencv2/opencv.hpp") + " /Fp" + q(p.pch);
            pch_obj = p.obj;
        }
    }
#endif

    cmd += "cl.exe /LD " + front;
    // Force-include the convenience header AFTER the PCH boundary. Script gets the
    // inspect_entry plumbing; plugin gets the C ABI export macros.
    if (req.mode == CompileMode::Script) cmd += " /FIxi/xi_script_support.hpp";
    else                                 cmd += " /FIxi/xi_plugin_support.hpp";
    // Positive OpenMP cap → bake it as a macro the support header reads at DLL
    // load (omp_set_num_threads). Post-PCH so the PCH isn't keyed per cap value.
    if (req.openmp_max_threads > 0)
        cmd += " /D XI_OMP_MAX_THREADS=" + std::to_string(req.openmp_max_threads);
    cmd += " /Fo" + q(req.output_dir + "\\\\");
    cmd += " /Fe" + q(out_dll.string());
    cmd += " " + q(req.source_path);
    // Additional source files
    for (auto& s : req.extra_sources) {
        cmd += " " + q(s);
    }
    cmd += " /link /IMPLIB:" + q((std::filesystem::path(req.output_dir) / (versioned_stem + ".lib")).string());
    // Emit a versioned program PDB matched to this DLL so crash
    // minidumps resolve script frames to file:line. /DEBUG makes the
    // linker write the debug directory into the DLL pointing at this
    // PDB (GUID+age matched). Versioned name → _vN retention keeps it
    // paired with the live DLL; no shared vc*.pdb contention.
    // PluginDev/PluginExport already carry /Zi; adding /DEBUG here is
    // harmless for them and required for the Script (/Z7) path.
    cmd += " /DEBUG /PDB:" + q((std::filesystem::path(req.output_dir)
                               / (versioned_stem + ".pdb")).string());
    // Link against the pre-built yyjson.lib from the backend's own build tree.
    // yyjson backs xi_json.hpp (the JSON/config/manifest layer) and any direct
    // #include "yyjson.h" in a script — NOT the long-deleted xi_record.hpp this
    // comment used to cite; the dependency outlived THE CUT. Round-3 W2 #10:
    // the path was hardcoded to build/Release/, so a Debug-only build tree
    // silently linked nothing and every yyjson_* symbol failed at script link
    // time. Ninja Multi-Config emits one lib per config subdir — probe Release
    // first (the shipped/dev-default config), then Debug.
    {
        auto build_dir = std::filesystem::path(req.include_dir).parent_path() / "build";
        for (const char* cfg : { "Release", "Debug" }) {
            auto yyjson_lib = build_dir / cfg / "yyjson.lib";
            if (std::filesystem::exists(yyjson_lib)) {
                cmd += " " + q(yyjson_lib.string());
                break;
            }
        }
    }
    // The PCH's own object (from /Yc) must be linked when consuming via /Yu.
    if (!pch_obj.empty()) cmd += " " + q(pch_obj);
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
                    cmd += " " + q(f.path().string());
                    break;
                }
            }
            break;
        }
    }
    if (!req.turbojpeg_root.empty()) {
        auto tj = std::filesystem::path(req.turbojpeg_root) / "lib" / "turbojpeg.lib";
        if (std::filesystem::exists(tj)) cmd += " " + q(tj.string());
    }
    if (!req.ipp_root.empty()) {
        for (auto& n : { "ippcore.lib", "ippi.lib", "ippcv.lib", "ippcc.lib" }) {
            auto p = std::filesystem::path(req.ipp_root) / "lib" / n;
            if (std::filesystem::exists(p)) cmd += " " + q(p.string());
        }
    }
    // User-declared import libs (project.json "link_libs"). Already resolved to
    // absolute paths by the caller. Lets a script/plugin link an external SDK
    // without a full-path #pragma comment(lib) in the source.
    for (auto& l : req.link_libs) cmd += " " + q(l);
    cmd += " > " + q(log_path.string()) + " 2>&1";
    cmd += "\"";

#ifdef _WIN32
    // Round-3 #6: no more std::system fallback — cl_env == nullptr spawns
    // bounded with the parent env inherited (the cmd line routes vcvars
    // inline), so a wedged toolchain is killed either way.
    int rc = detail::run_with_env(cmd, cl_env);
#else
    int rc = std::system(cmd.c_str());
#endif
    // Sanitize the build log for the WS text frame, which must be UTF-8. On a
    // non-English Windows cl.exe emits localized diagnostics in the local code
    // page; ensure_utf8 transcodes them so they're never mojibake on the wire.
    // (TODO(linux): gcc/clang emit UTF-8 already; ensure_utf8 is a no-op there.)
    r.build_log = ensure_utf8(detail::read_file(log_path.string()));
#ifdef _WIN32
    if (rc == -2) {
        // run_with_env killed a wedged toolchain (see kCompileTimeoutMs). Whatever
        // partial log survives is prefixed with a plain-language diagnostic so the
        // client sees a "toolchain timed out" failure rather than a silent stall.
        r.build_log = "[xi] toolchain timed out after " +
                      std::to_string(detail::kCompileTimeoutMs / 1000) +
                      "s and was terminated\n" + r.build_log;
    }
#endif

    if (rc == 0 && std::filesystem::exists(out_dll)) {
        r.ok = true;
        r.dll_path = out_dll.string();

        // Audit P0-E2 (Q2 decision: delete-on-success): on a clean
        // compile, prune older `<stem>_v<N>.{dll,lib,obj,log,exp,pdb}`
        // siblings of the same source. ~5 GB/day at typical dev
        // cadence accumulates to 150 GB after 30 days; keeping just
        // the latest fixes that.
        //
        // We keep:
        //  - the just-written ver (current `ver`)
        //  - the actual PREVIOUS same-stem version = the highest `<stem>_v<n>`
        //    with n < ver present on disk. This prune runs INSIDE compile(),
        //    BEFORE load_script + the g_eng.script swap, so that previous build
        //    is still the LIVE mapped script; its .dll delete fails safely
        //    (mapped → sharing violation, ignored) but deleting its
        //    .pdb/.lib/.exp/.obj would leave a crash in the swap window unable
        //    to resolve the old script's frames. NOTE (M2): it is NOT `ver-1` —
        //    `s_version` is a single process-global counter shared across ALL
        //    compile modes (inspect scripts + project plugins + AOT), so an
        //    intervening plugin build makes the real previous script build
        //    `ver-2` or lower. We scan for it per stem instead of assuming
        //    consecutive numbering.
        //  - any file whose <stem>_v prefix doesn't match (defensive
        //    — different scripts share the build dir)
        //
        // Failed deletes (e.g. AV scanner has the file open, or
        // FreeLibrary hasn't released a previous .dll yet) are
        // ignored — they'll get cleaned up on the next compile.
        std::error_code prune_ec;
        std::string prefix = stem + "_v";
        auto out_dir = std::filesystem::path(req.output_dir);
        // Parse `<stem>_v<n>.<ext>` → n, or -1 if it doesn't match this stem.
        auto parse_stem_ver = [&](const std::string& fn) -> int {
            if (fn.size() <= prefix.size() || fn.compare(0, prefix.size(), prefix) != 0) return -1;
            size_t av = prefix.size(); size_t d = fn.find('.', av);
            if (d == std::string::npos || d == av) return -1;
            try { return std::stoi(fn.substr(av, d - av)); } catch (...) { return -1; }
        };
        if (std::filesystem::exists(out_dir, prune_ec)) {
            // M2: the real previous same-stem version is the highest n < ver on disk,
            // NOT `ver-1` (the global s_version counter is bumped by plugin/AOT builds
            // too, so script versions are not consecutive per stem).
            int prev_ver = -1;
            for (auto& entry : std::filesystem::directory_iterator(
                     out_dir, std::filesystem::directory_options::skip_permission_denied, prune_ec)) {
                if (prune_ec) break;
                if (!entry.is_regular_file(prune_ec)) continue;
                int fv = parse_stem_ver(entry.path().filename().string());
                if (fv >= 0 && fv < ver && fv > prev_ver) prev_ver = fv;
            }
            for (auto& entry : std::filesystem::directory_iterator(
                     out_dir, std::filesystem::directory_options::skip_permission_denied, prune_ec)) {
                if (prune_ec) break;
                if (!entry.is_regular_file(prune_ec)) continue;
                auto fname = entry.path().filename().string();
                // Only consider files matching `<stem>_v<digits>.<ext>`
                if (fname.size() <= prefix.size()) continue;
                if (fname.compare(0, prefix.size(), prefix) != 0) continue;
                size_t after_v = prefix.size();
                size_t dot     = fname.find('.', after_v);
                if (dot == std::string::npos || dot == after_v) continue;
                // Extract version number
                int file_ver = -1;
                try {
                    file_ver = std::stoi(fname.substr(after_v, dot - after_v));
                } catch (...) { continue; }
                if (file_ver == ver) continue;       // keep current (N)
                if (file_ver == prev_ver) continue;  // keep the real previous same-stem version (M2)
                std::error_code rm_ec;
                std::filesystem::remove(entry.path(), rm_ec);
                // Best-effort: ignore rm_ec (AV lock, etc.) — next
                // compile will retry.
            }
        }
    }
    r.diagnostics = parse_diagnostics(r.build_log);
    augment_var_redefinitions(r.diagnostics, req.source_path);
    return r;
}

} // namespace xi::script
