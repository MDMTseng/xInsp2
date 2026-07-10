//
// test_certify.cpp — Part III G1 scan/certification isolation.
//
// Proves the throwaway-child-process certify path end to end through the REAL
// PluginManager::scan_plugins gate:
//   * a known-good plugin (the golden DLL) certifies `ok` and IS registered;
//   * a deliberately-crashing plugin (certify_crash_plugin — factory derefs null)
//     certifies `crashed`, is SKIPPED by scan_plugins, and is surfaced via
//     certify_warnings(); the backend process never loaded the bad DLL;
//   * the verdict is cached by DLL content hash, so a re-scan with certification
//     DISABLED still honours the cached `crashed` (discovery can't re-arm it).
//
// This same executable is BOTH the test driver and the certify child: invoked
// with `--certify-plugin <dir>` it runs xi::certify::certify_in_process and
// exits with the verdict code (self-reexec — no dependency on the runner/backend
// being built or findable). Mirrors the golden/teardown fixture patterns.
//
#include <xi/xi_certify.hpp>
#include <xi/xi_crash_dump.hpp>
#include <xi/xi_plugin_manager.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

#ifndef GOLDEN_PLUGIN_DLL
#define GOLDEN_PLUGIN_DLL "golden_plugin.dll"
#endif
#ifndef CRASH_PLUGIN_DLL
#define CRASH_PLUGIN_DLL "certify_crash_plugin.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static std::string self_exe() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::string(buf, n);
}

// Lay down <root>/<name>/{plugin.json, <name>.dll} from a source DLL.
static void make_plugin_folder(const fs::path& root, const std::string& name,
                               const std::string& src_dll) {
    fs::path dir = root / name;
    fs::create_directories(dir);
    std::ofstream mf((dir / "plugin.json").string(), std::ios::binary | std::ios::trunc);
    mf << "{\"name\":\"" << name << "\",\"dll\":\"" << name
       << ".dll\",\"factory\":\"xi_plugin_create\"}\n";
    mf.close();
    std::error_code ec;
    fs::copy_file(src_dll, dir / (name + ".dll"),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) std::fprintf(stderr, "FAIL: copy %s -> %s: %s\n", src_dll.c_str(),
                         (dir / (name + ".dll")).string().c_str(), ec.message().c_str());
}

int main(int argc, char** argv) {
    // Self-reexec as the certify child: load the DLL + call its factory once in
    // THIS process and exit with the verdict code. A crash here yields a minidump.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--certify-plugin") {
            const char* dir = (i + 1 < argc) ? argv[i + 1] : nullptr;
            xi::crash::install();
            int code = dir ? xi::certify::certify_in_process(dir)
                           : xi::certify::kExitAbiMismatch;
            std::fflush(stderr);
            std::fflush(stdout);
            return code;
        }
    }

    std::printf("[test] certify isolation — child-process certify + scan gate\n");

    const std::string exe = self_exe();
    fs::path root = fs::temp_directory_path() /
                    ("xinsp2_certify_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    make_plugin_folder(root, "good_cert",  GOLDEN_PLUGIN_DLL);
    make_plugin_folder(root, "bad_cert",   CRASH_PLUGIN_DLL);

    // (A) Direct verdict protocol — the subprocess maps exit codes correctly.
    {
        auto good = xi::certify::run_certify_subprocess(exe, (root / "good_cert").string());
        auto bad  = xi::certify::run_certify_subprocess(exe, (root / "bad_cert").string());
        std::printf("  direct verdicts: good=%s bad=%s\n",
                    xi::certify::verdict_str(good), xi::certify::verdict_str(bad));
        CHECK(good == xi::certify::Verdict::ok);
        CHECK(bad  == xi::certify::Verdict::crashed);
    }

    // (B) The real scan gate: good registers, bad is skipped + surfaced, and the
    // bad DLL is never loaded into THIS process.
    {
        xi::PluginManager pm;
        pm.set_certify_exe(exe);
        // QuiesceToken: bare PluginManager, single-threaded test — no dispatch pool.
        int n = pm.scan_plugins(xi::QuiesceToken::assert_no_dispatch(), root.string());
        std::printf("  scan_plugins registered %d plugin(s)\n", n);

        CHECK(pm.find_plugin("good_cert") != nullptr);   // good armed
        CHECK(pm.find_plugin("bad_cert")  == nullptr);   // bad gated out
        CHECK(n == 1);

        auto warns = pm.certify_warnings();
        bool surfaced = false;
        for (auto& w : warns) if (w.plugin == "bad_cert") surfaced = true;
        CHECK(surfaced);   // G1.3 surfaces the skip
    }

    // (C) The verdict was cached by DLL content hash next to the manifest (G1.2).
    {
        xi::certify::CacheEntry e;
        CHECK(xi::certify::read_cache((root / "bad_cert").string(), e));
        CHECK(e.verdict == "crashed");
        CHECK(e.dll == "bad_cert.dll");
        CHECK(!e.sha256.empty());

        xi::certify::CacheEntry g;
        CHECK(xi::certify::read_cache((root / "good_cert").string(), g));
        CHECK(g.verdict == "ok");
    }

    // (D) Discovery can never re-arm a known-bad DLL: a fresh manager with
    // certification DISABLED (no certify_exe_) still skips bad_cert purely on the
    // cached `crashed` verdict — no child spawned.
    {
        xi::PluginManager pm2;   // certify_exe_ empty
        int n = pm2.scan_plugins(xi::QuiesceToken::assert_no_dispatch(), root.string());
        CHECK(pm2.find_plugin("good_cert") != nullptr);
        CHECK(pm2.find_plugin("bad_cert")  == nullptr);
        CHECK(n == 1);
    }

    fs::remove_all(root, ec);

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
