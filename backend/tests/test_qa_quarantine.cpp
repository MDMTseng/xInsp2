//
// test_qa_quarantine.cpp — Part III G2: per-plugin crash attribution + auto-disable.
//
// G2 graduates the supervisor's crash lever from process-granular ("respawn /
// latch the whole backend") to plugin-granular ("disable the one offending
// plugin, keep the line up"). This pins the three pure/unit-testable pieces of
// that loop in isolation; the full crash -> death -> respawn -> disable path is
// integration-heavy and exercised by the FE supervisor end to end (acknowledged
// integration-level below).
//
//   G2.1 attribution — crash_history_line emits the culprit; enrich_from_crash_
//        report joins the process-global culprit to the faulting module (and
//        REFUSES to attribute a script/core crash whose module doesn't match).
//   G2.2 counter     — PluginFaultTracker trips per plugin within a window.
//   G2.2 quarantine  — the REAL PluginManager::scan_plugins gate skips a plugin
//        whose .xi_certify.json verdict is "quarantined" (reusing G1's mechanism)
//        and surfaces it; the FE's exact on-disk schema round-trips.
//   G2.3 un-quarantine — unquarantine_plugin() clears it; a DLL CONTENT-HASH
//        change clears it automatically (the G1.2 hash gate).
//   Race  — the process-global culprit stamp (xi::crash::set_culprit) is hit by
//        many dispatch threads at once; stressed under ASan (TSan unavailable on
//        this box — OQ-1) via XINSP2_STRESS_SCALE.
//
#include <xi/xi_safe_state.hpp>
#include <xi/xi_crash_history.hpp>
#include <xi/xi_crash_report.hpp>
#include <xi/xi_crash_dump.hpp>
#include <xi/xi_respawn_policy.hpp>
#include <xi/xi_certify.hpp>
#include <xi/xi_plugin_manager.hpp>
#include <xi/xi_sha256.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

#ifndef GOLDEN_PLUGIN_DLL
#define GOLDEN_PLUGIN_DLL "golden_plugin.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define CHECK_NOTHROW(stmt)                                                    \
    do {                                                                       \
        try { stmt; }                                                          \
        catch (...) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: threw\n", __FILE__, __LINE__);   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static void write_file(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

static fs::path scratch(const char* tag) {
    fs::path d = fs::temp_directory_path() /
        ("xinsp2-qa-quar-" + std::to_string(GetCurrentProcessId()) + "-" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

// Lay down <root>/<name>/{plugin.json, <name>.dll} from the golden source DLL.
static void make_plugin_folder(const fs::path& root, const std::string& name) {
    fs::path dir = root / name;
    fs::create_directories(dir);
    write_file(dir / "plugin.json",
               "{\"name\":\"" + name + "\",\"dll\":\"" + name +
               ".dll\",\"factory\":\"xi_plugin_create\"}\n");
    std::error_code ec;
    fs::copy_file(GOLDEN_PLUGIN_DLL, dir / (name + ".dll"),
                  fs::copy_options::overwrite_existing, ec);
}

// Write G1's .xi_certify.json verdict for a plugin folder, keyed by the DLL's
// CURRENT content hash — exactly what the FE's quarantine_plugin() writes.
static void write_verdict(const fs::path& dir, const std::string& dll,
                          const std::string& verdict) {
    std::string sha = xi::sha256::sha256_file((dir / dll).string());
    write_file(dir / ".xi_certify.json",
               "{\"dll\":\"" + dll + "\",\"sha256\":\"" + sha +
               "\",\"verdict\":\"" + verdict + "\"}\n");
}

int main() {
    using xi::SafeStateEvent;
    std::printf("[test] G2 per-plugin crash attribution + quarantine\n");

    // ===================================================================
    // G2.1 — crash_history_line emits the culprit fields.
    // ===================================================================
    {
        SafeStateEvent ev;
        ev.faulting_module  = "flaky.dll+0x12";
        ev.culprit_plugin   = "flaky";
        ev.culprit_instance = "flaky_1";
        std::string line = xi::crash_history_line(ev, /*consecutive=*/2, /*cap_hit=*/false);
        CHECK(line.find("\"culprit_plugin\":\"flaky\"") != std::string::npos);
        CHECK(line.find("\"culprit_instance\":\"flaky_1\"") != std::string::npos);
        // Empty culprit (unattributed death) still yields valid empty fields.
        SafeStateEvent ev2;
        std::string line2 = xi::crash_history_line(ev2, 1, false);
        CHECK(line2.find("\"culprit_plugin\":\"\"") != std::string::npos);
    }

    // ===================================================================
    // G2.1 — enrich attributes to the culprit ONLY when the faulting module
    // matches the stamped dll (so a script/core crash isn't mis-blamed).
    // ===================================================================
    {
        // (a) culprit dll matches faulting module -> attributed + folder carried.
        fs::path d = scratch("enrich_match");
        fs::path dmp = d / "be.dmp";
        write_file(dmp, "x");
        write_file(fs::path(dmp).replace_extension(".json"),
                   R"({"exception":{"name":"ACCESS_VIOLATION","module":"flaky.dll+0x40"},)"
                   R"("context":{"last_phase":"inspect"},)"
                   R"("culprit":{"instance":"flaky_1","plugin":"flaky",)"
                   R"("folder":"C:\\plugins\\flaky","dll":"flaky.dll"}})");
        write_file(d / "be.log", "minidump: " + dmp.string() + "\n");
        SafeStateEvent ev;
        CHECK_NOTHROW(xi::enrich_from_crash_report((d / "be.log").string(), ev));
        CHECK(ev.culprit_plugin   == "flaky");
        CHECK(ev.culprit_instance == "flaky_1");
        CHECK(ev.culprit_dll      == "flaky.dll");
        CHECK(ev.culprit_folder.find("flaky") != std::string::npos);
    }
    {
        // (b) faulting module is the CORE/script (not the plugin dll) -> NOT
        // attributed, even though a culprit was stamped. This is the guard that
        // stops a stale stamp mis-quarantining an innocent plugin.
        fs::path d = scratch("enrich_mismatch");
        fs::path dmp = d / "be.dmp";
        write_file(dmp, "x");
        write_file(fs::path(dmp).replace_extension(".json"),
                   R"({"exception":{"name":"ACCESS_VIOLATION","module":"xinsp-backend.exe+0x99"},)"
                   R"("culprit":{"instance":"flaky_1","plugin":"flaky",)"
                   R"("folder":"C:\\plugins\\flaky","dll":"flaky.dll"}})");
        write_file(d / "be.log", "minidump: " + dmp.string() + "\n");
        SafeStateEvent ev;
        CHECK_NOTHROW(xi::enrich_from_crash_report((d / "be.log").string(), ev));
        CHECK(ev.culprit_plugin.empty());   // module mismatch -> no attribution
        CHECK(ev.culprit_folder.empty());
    }

    // ===================================================================
    // G2.2 — PluginFaultTracker: per-plugin budget within a window.
    // ===================================================================
    {
        xi::PluginFaultTracker t;
        t.max = 3; t.window_ms = 1000;
        // 2 faults: under budget, no trip.
        CHECK(!t.note_fault("flaky", 0));
        CHECK(!t.note_fault("flaky", 100));
        CHECK(t.count("flaky") == 2);
        CHECK(!t.tripped("flaky"));
        // 3rd within the window: TRIPS exactly once.
        CHECK(t.note_fault("flaky", 200));
        CHECK(t.tripped("flaky"));
        // A 4th fault does NOT re-trip (already quarantined; act once).
        CHECK(!t.note_fault("flaky", 250));

        // A different plugin has an independent budget.
        xi::PluginFaultTracker t2; t2.max = 2; t2.window_ms = 1000;
        CHECK(!t2.note_fault("a", 0));
        CHECK(!t2.note_fault("b", 0));   // b's first, independent of a
        CHECK(t2.note_fault("a", 10));   // a's second -> trips
        CHECK(!t2.tripped("b"));

        // Sliding window: faults spaced beyond the window never accumulate.
        xi::PluginFaultTracker t3; t3.max = 3; t3.window_ms = 1000;
        CHECK(!t3.note_fault("slow", 0));
        CHECK(!t3.note_fault("slow", 5000));    // > window since last -> count resets to 1
        CHECK(!t3.note_fault("slow", 10000));   // resets again -> still 1
        CHECK(!t3.tripped("slow"));

        // clear() forgets a tripped plugin (un-quarantine / rebuild).
        CHECK(t.tripped("flaky"));
        t.clear("flaky");
        CHECK(!t.tripped("flaky"));
        CHECK(t.count("flaky") == 0);
    }

    // ===================================================================
    // G2.2 — the REAL scan gate skips a quarantined plugin + surfaces it,
    // reusing G1's .xi_certify.json (no second disable path). G2.3 —
    // unquarantine_plugin() + a DLL hash change both clear it.
    // ===================================================================
    {
        fs::path root = scratch("scan_gate");
        make_plugin_folder(root, "quar");   // golden DLL, certifies fine normally
        make_plugin_folder(root, "good");
        // Pin "quar" as quarantined for its current DLL hash (what the FE writes).
        write_verdict(root / "quar", "quar.dll", "quarantined");

        // (a) scan with certification DISABLED honours the cached quarantine purely
        // from disk (no child spawned) — discovery never arms the bad plugin.
        {
            xi::PluginManager pm;   // certify_exe_ empty
            int n = pm.scan_plugins(root.string());
            CHECK(pm.find_plugin("quar") == nullptr);   // gated out
            CHECK(pm.find_plugin("good") != nullptr);    // sibling unaffected
            CHECK(n == 1);
            bool surfaced = false;
            for (auto& w : pm.certify_warnings())
                if (w.plugin == "quar" && w.reason.find("QUARANTINED") != std::string::npos)
                    surfaced = true;
            CHECK(surfaced);   // operator sees WHICH plugin + why

            // (b) G2.3 operator un-quarantine: clears the verdict, re-scan re-arms.
            CHECK(pm.unquarantine_plugin("quar"));
            int n2 = pm.scan_plugins(root.string());
            CHECK(pm.find_plugin("quar") != nullptr);    // now armed
            CHECK(n2 == 2);
        }

        // (c) G2.3 hash-gated auto-clear: re-quarantine, then CHANGE the DLL bytes
        // (a rebuild). The cached verdict's sha no longer matches, so the gate
        // ignores it and the plugin re-arms without an explicit un-quarantine.
        {
            write_verdict(root / "quar", "quar.dll", "quarantined");
            xi::PluginManager pm;
            CHECK(pm.scan_plugins(root.string()) == 1);   // gated again
            CHECK(pm.find_plugin("quar") == nullptr);
            // Mutate the DLL on disk -> new content hash.
            { std::ofstream f((root / "quar" / "quar.dll").string(),
                              std::ios::binary | std::ios::app); f << "REBUILT"; }
            xi::PluginManager pm2;
            int n = pm2.scan_plugins(root.string());
            CHECK(pm2.find_plugin("quar") != nullptr);    // stale verdict ignored
            CHECK(n == 2);
        }
        std::error_code ec; fs::remove_all(root, ec);
    }

    // ===================================================================
    // Race — the process-global culprit stamp is written by many dispatch
    // threads concurrently. ASan + XINSP2_STRESS_SCALE hammers the strncpy
    // paths (TSan unavailable, OQ-1); the bound is that no write/read ever runs
    // off the fixed buffers and the fields stay NUL-terminated.
    // ===================================================================
    {
        long long scale = 1;
        if (const char* e = std::getenv("XINSP2_STRESS_SCALE")) {
            long long v = std::atoll(e); if (v > 0) scale = v;
        }
        const long long N = 20000 * scale;
        std::atomic<bool> stop{false};
        std::atomic<long long> reads{0};

        // Reader: mimics the crash handler reading g_culprit on the faulting
        // thread — must always see NUL-terminated, in-bounds strings.
        std::thread reader([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                volatile size_t li = ::strnlen(xi::crash::g_culprit.instance,
                                               sizeof(xi::crash::g_culprit.instance));
                volatile size_t lp = ::strnlen(xi::crash::g_culprit.plugin,
                                               sizeof(xi::crash::g_culprit.plugin));
                CHECK(li <= sizeof(xi::crash::g_culprit.instance));
                CHECK(lp <= sizeof(xi::crash::g_culprit.plugin));
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::vector<std::thread> writers;
        for (int w = 0; w < 4; ++w) {
            writers.emplace_back([&, w] {
                for (long long i = 0; i < N; ++i) {
                    std::string inst = "inst_" + std::to_string(w) + "_" + std::to_string(i & 0xFF);
                    std::string plug = "plug_" + std::to_string((i * 7 + w) & 0x3F);
                    xi::crash::set_culprit(inst.c_str(), plug.c_str(),
                                           "C:\\plugins\\p", "p.dll");
                }
            });
        }
        for (auto& t : writers) t.join();
        stop.store(true, std::memory_order_relaxed);
        reader.join();
        CHECK(reads.load() > 0);
        std::printf("  culprit-stamp race: %lld stamps x4 threads, %lld reads (scale=%lld)\n",
                    (long long)N, reads.load(), scale);
    }

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
