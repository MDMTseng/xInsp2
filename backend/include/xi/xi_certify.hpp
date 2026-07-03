#pragma once
//
// xi_certify.hpp — Part III G1: scan / certification isolation.
//
// Crash-safety for the riskiest moment in the in-process runtime: the FIRST
// load of untrusted third-party native code during discovery. Today
// scan_plugins -> register_plugin_folder -> LoadLibraryEx + factory-resolve runs
// in the BACKEND process, so a malformed DLL whose DllMain or factory faults
// takes the whole backend down at discovery time (and the FE respawn re-scans
// straight back into the same crash). G1 moves that first load+instantiate into
// a THROWAWAY CHILD PROCESS: the child loads the DLL and calls the factory once
// (create -> destroy); a fault crashes only the child, never the backend.
//
// This is CRASH SAFETY, NOT supply-chain trust (see OPEN-QUESTIONS OQ-6):
// plugins remain "trusted", signing stays a deliberate non-goal. The in-process
// RUNTIME bet (core_fix_plan §5.1 / §20.1) is untouched — process isolation
// returns ONLY for scan/certify, never the dispatch hot path.
//
// Verdict protocol (child exit code -> parent verdict):
//   kExitOk        (0)  -> ok            factory create+destroy survived cleanly
//   kExitAbiMismatch(42)-> abi_mismatch  ABI/yyjson gate refused, or the DLL
//                                         could not be loaded / factory missing,
//                                         or the factory cleanly refused (null /
//                                         caught C++ exception). Loadable-but-not-
//                                         ok, but it did NOT crash the host.
//   anything else / abnormal termination -> crashed   a hard fault (SEH access
//                                         violation, abort, stack overflow, ...)
//                                         terminated the child with its own code;
//                                         a hang past the timeout is killed and
//                                         also read as crashed.
//
// Only `crashed` gates discovery (xi_plugin_manager scan_plugins): a plugin that
// crashed a throwaway child is the exact "would have killed the backend" case.
// `abi_mismatch` is NOT gated — the real load path already refuses it with a
// reason, and a factory that merely returns null never endangered the backend.
//
// The child reuses xi_crash_dump so a crashed certify still leaves a minidump +
// JSON sidecar for forensics (the caller installs xi::crash::install() before
// invoking certify_in_process). The child deliberately does NOT install the SEH
// translator: a fault must reach the unhandled-exception filter (-> minidump ->
// terminate) rather than being caught and swallowed.
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

#include "xi_abi.h"
#include "xi_cabi_adapter.hpp"   // PluginInfo, plugin_abi_compatible
#include "xi_image_pool.hpp"     // ImagePool::make_host_api
#include "xi_pm_parse.hpp"       // parse_manifest, extract_string
#include "xi_sha256.hpp"         // sha256_file (content-hash cache key)
#include "xi_trigger_bus.hpp"    // install_trigger_hook
#include "xi_pack_abi.hpp"      // polaris2 wave-2: install_pack_abi (xi.pack@1 door)
#include "xi_cap_abi.hpp"       // capability plane pilot (doc 14): install_cap_plane

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace xi::certify {

// `quarantined` (Part III G2.2) reuses this SAME cache file + scan gate as the
// G1 `crashed` verdict, but is written by the FE SUPERVISOR after a plugin is
// attributed N runtime crashes (not by the discovery-time certify child). It is
// keyed by the same DLL content hash, so rebuilding the DLL (a new hash) clears
// it automatically (G1.2 re-certifies on hash change) — that is G2.3's auto
// un-quarantine. One mechanism, two writers (Invariant §20.3).
enum class Verdict { ok, abi_mismatch, crashed, quarantined, unknown };

// Child exit codes. `crashed` is intentionally NOT a fixed code — it is whatever
// abnormal value a hard fault terminates the child with, so any exit that is
// neither ok nor abi_mismatch is read as crashed.
constexpr int kExitOk          = 0;
constexpr int kExitAbiMismatch = 42;

inline const char* verdict_str(Verdict v) {
    switch (v) {
        case Verdict::ok:           return "ok";
        case Verdict::abi_mismatch: return "abi_mismatch";
        case Verdict::crashed:      return "crashed";
        case Verdict::quarantined:  return "quarantined";
        default:                    return "unknown";
    }
}

inline Verdict verdict_from_str(const std::string& s) {
    if (s == "ok")           return Verdict::ok;
    if (s == "abi_mismatch") return Verdict::abi_mismatch;
    if (s == "crashed")      return Verdict::crashed;
    if (s == "quarantined")  return Verdict::quarantined;
    return Verdict::unknown;
}

// ------------------------------------------------------------------------
// Child side: load the plugin DLL and call its factory once, in THIS (child)
// process. Returns the exit code the child should exit with. Never installs a
// SEH translator — a fault propagates to xi_crash_dump's unhandled filter.
// ------------------------------------------------------------------------
inline int certify_in_process(const std::string& plugin_dir) {
    namespace fs = std::filesystem;
    auto manifest = fs::path(plugin_dir) / "plugin.json";
    if (!fs::exists(manifest)) return kExitAbiMismatch;
    auto info = parse_manifest(manifest.string(), plugin_dir);
    if (info.name.empty()) return kExitAbiMismatch;
    auto dll_path = (fs::path(plugin_dir) / info.dll_name).string();
    if (!fs::exists(dll_path)) return kExitAbiMismatch;

#ifdef _WIN32
    // Same load primitive the real loader uses (deps resolve from the plugin's
    // own folder). A DllMain that returns FALSE -> nullptr here (clean load
    // failure = abi_mismatch); a DllMain that FAULTS already terminated us
    // (-> crashed) before we get a handle back.
    HMODULE h = LoadLibraryExA(dll_path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!h) {
        std::fprintf(stderr, "[certify] LoadLibrary failed for %s (err %lu)\n",
                     dll_path.c_str(), GetLastError());
        return kExitAbiMismatch;
    }

    std::string aerr;
    if (!plugin_abi_compatible(h, info.name, info.json_fallback, &aerr)) {
        std::fprintf(stderr, "[certify] %s\n", aerr.c_str());
        FreeLibrary(h);
        return kExitAbiMismatch;
    }

    auto factory = reinterpret_cast<PluginInfo::CFactoryFn>(
        GetProcAddress(h, info.factory_symbol.c_str()));
    if (!factory) {
        std::fprintf(stderr, "[certify] factory '%s' not found in %s\n",
                     info.factory_symbol.c_str(), info.dll_name.c_str());
        FreeLibrary(h);
        return kExitAbiMismatch;
    }

    // A real host_api backed by the live ImagePool + trigger hook — exactly what
    // the backend hands a plugin at create(). The riskiest single moment: a
    // fault inside factory() terminates this child (-> minidump -> crashed).
    xi_host_api host = ImagePool::make_host_api();
    install_trigger_hook(host);
    install_pack_abi();   // polaris2 wave-2: certify a pack-capable plugin against a live xi.pack@1 door
    install_cap_plane();  // doc 14 pilot: certify a lib plugin against a live registration door
                          // (this child process is throwaway — registrations die with it)

    void* inst = nullptr;
    // Catch ONLY genuine C++ exceptions (a clean refuse — the real factory sites
    // catch too, so a throwing factory never crashes the backend). Deliberately
    // NOT catch(...): MSVC builds with /EHa, under which catch(...) would also
    // swallow a hardware SEH fault (access violation, etc.) — exactly the crash
    // we must let propagate to xi_crash_dump's unhandled filter (-> minidump ->
    // abnormal exit -> the parent reads `crashed`).
    try {
        inst = factory(&host, "__xi_certify_probe__");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[certify] factory threw a C++ exception (clean refuse): %s\n",
                     e.what());
        FreeLibrary(h);
        return kExitAbiMismatch;
    }
    if (!inst) {
        std::fprintf(stderr, "[certify] factory returned null (clean refuse)\n");
        FreeLibrary(h);
        return kExitAbiMismatch;
    }

    // Destroy through the DLL's own destroy export (must run before FreeLibrary).
    auto destroy = reinterpret_cast<xi_plugin_destroy_fn>(
        GetProcAddress(h, "xi_plugin_destroy"));
    if (destroy) destroy(inst);   // a fault here -> crashed via the unhandled filter
    FreeLibrary(h);
    std::fprintf(stderr, "[certify] '%s' OK\n", info.name.c_str());
    return kExitOk;
#else
    // TODO(linux): dlopen + factory in a fork()'d child.
    return kExitOk;
#endif
}

#ifdef _WIN32
// ------------------------------------------------------------------------
// Parent side: spawn `certify_exe --certify-plugin <plugin_dir>`, wait, map the
// child's exit code to a verdict. `certify_exe` is any binary that handles the
// --certify-plugin mode (the backend, the runner, or a test driver self-reexec).
// ------------------------------------------------------------------------
inline Verdict run_certify_subprocess(const std::string& certify_exe,
                                      const std::string& plugin_dir,
                                      uint32_t timeout_ms = 30000) {
    std::string cmd = "\"" + certify_exe + "\" --certify-plugin \"" + plugin_dir + "\"";
    std::vector<char> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "[certify] CreateProcess failed for %s (err %lu)\n",
                     certify_exe.c_str(), GetLastError());
        return Verdict::unknown;   // could not certify — do NOT gate on this
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        // A factory that hangs is as dangerous to discovery as one that crashes.
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        std::fprintf(stderr, "[certify] '%s' timed out after %ums -> crashed\n",
                     plugin_dir.c_str(), timeout_ms);
        return Verdict::crashed;
    }

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (code == (DWORD)kExitOk)          return Verdict::ok;
    if (code == (DWORD)kExitAbiMismatch) return Verdict::abi_mismatch;
    return Verdict::crashed;   // any other / abnormal exit code = a hard fault
}
#endif // _WIN32

// ------------------------------------------------------------------------
// Verdict cache — keyed by DLL content hash, stored next to the manifest, so a
// normal startup re-certifies ONLY when the DLL changes (G1.2).
// ------------------------------------------------------------------------
struct CacheEntry {
    std::string dll;       // dll_name the verdict applies to
    std::string sha256;    // content hash of that DLL
    std::string verdict;   // verdict_str(...)
};

inline std::string cache_path(const std::string& plugin_dir) {
    return (std::filesystem::path(plugin_dir) / ".xi_certify.json").string();
}

inline bool read_cache(const std::string& plugin_dir, CacheEntry& out) {
    std::ifstream f(cache_path(plugin_dir));
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string js = ss.str();
    if (js.empty()) return false;
    out.dll     = extract_string(js, "dll").value_or("");
    out.sha256  = extract_string(js, "sha256").value_or("");
    out.verdict = extract_string(js, "verdict").value_or("");
    return !out.sha256.empty() && !out.verdict.empty();
}

inline void write_cache(const std::string& plugin_dir, const CacheEntry& e) {
    std::string js = "{\"dll\":\"" + e.dll +
                     "\",\"sha256\":\"" + e.sha256 +
                     "\",\"verdict\":\"" + e.verdict + "\"}\n";
    std::ofstream f(cache_path(plugin_dir), std::ios::binary | std::ios::trunc);
    if (f) f << js;   // best-effort: a read-only plugin folder just re-certifies next time
}

} // namespace xi::certify
