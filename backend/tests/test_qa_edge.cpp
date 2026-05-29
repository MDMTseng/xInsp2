//
// test_qa_edge.cpp — QA Agent EDGE unit for the FE crash-report parser.
//
// Viewpoint: resource / security / edge cases (test plan §1 CR-U*). This pins
// xi::enrich_from_crash_report (proposed extraction of fe_main.cpp's file-static
// enrich_from_crash_report into the portable header xi_crash_report.hpp). The
// parser is the bit of the FE that turns a dead backend's log into the forensic
// fields an operator/PLC sees in the safe-state line, so its NEGATIVE behaviour
// (no minidump line, missing/corrupt json, multiple lines) matters as much as
// the happy path: it must never throw and must degrade to empty fields.
//
// Self-contained: each case writes its own be.log + sibling .dmp/.json into a
// unique temp dir, so the minidump path the log names resolves to a real file
// the parser reads — exactly the runtime contract. No process spawning, no
// network; mirrors the CHECK(cond) + tmpfile style of test_safe_state.cpp.
//
// Build: see CMake add lines in the EDGE report. Run: test_qa_edge[.exe].
//
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <xi/xi_crash_report.hpp>
#include <xi/xi_safe_state.hpp>

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// A throw out of the parser is itself a failure (the FE calls it on the death
// path and must not let an exception abort going safe).
#define CHECK_NOTHROW(stmt)                                                    \
    do {                                                                       \
        try { stmt; }                                                          \
        catch (const std::exception& e) {                                     \
            std::fprintf(stderr, "FAIL %s:%d: threw %s\n", __FILE__, __LINE__, e.what()); \
            ++g_failures;                                                      \
        } catch (...) {                                                        \
            std::fprintf(stderr, "FAIL %s:%d: threw (non-std)\n", __FILE__, __LINE__); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static void write_file(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

// Make a unique scratch dir for one case so sibling .dmp/.json resolve cleanly.
static fs::path scratch(const char* tag) {
    fs::path d = fs::temp_directory_path() / ("xinsp2-qa-edge-" + std::string(tag));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

int main() {
    using xi::SafeStateEvent;

    // CR-U1: parse a known-good report -> fills exception/module/last_phase.
    {
        fs::path d = scratch("u1");
        fs::path dmp = d / "xinsp-backend-1234-20260530.dmp";
        write_file(dmp, std::string("MDMP", 4));  // contents irrelevant; parser only needs the path
        write_file(fs::path(dmp).replace_extension(".json"),
                   R"({"exception":{"name":"ACCESS_VIOLATION","module":"plugin_v0.dll"},)"
                   R"("context":{"last_phase":"inspect"}})");
        write_file(d / "be.log",
                   "[backend] booting\n"
                   "[backend] FATAL — minidump: " + dmp.string() + "\n");
        SafeStateEvent ev;
        CHECK_NOTHROW(xi::enrich_from_crash_report((d / "be.log").string(), ev));
        CHECK(ev.exception_name == "ACCESS_VIOLATION");
        CHECK(ev.faulting_module == "plugin_v0.dll");
        CHECK(ev.last_phase == "inspect");
        CHECK(!ev.report_path.empty());
        CHECK(fs::path(ev.report_path).extension() == ".json");
    }

    // CR-U2: context.last_phase empty -> threads[] fallback, prefers "inspect".
    // The first thread carries a non-empty breadcrumb ("idle"); a later one
    // carries "inspect" — the parser must end on "inspect", not the first hit.
    {
        fs::path d = scratch("u2");
        fs::path dmp = d / "be-crash.dmp";
        write_file(dmp, "x");
        write_file(fs::path(dmp).replace_extension(".json"),
                   R"({"exception":{"name":"ACCESS_VIOLATION"},)"
                   R"("context":{"last_phase":""},)"
                   R"("threads":[{"last_phase":"idle"},{"last_phase":"inspect"},{"last_phase":"teardown"}]})");
        write_file(d / "be.log", "minidump: " + dmp.string() + "\n");
        SafeStateEvent ev;
        CHECK_NOTHROW(xi::enrich_from_crash_report((d / "be.log").string(), ev));
        CHECK(ev.last_phase == "inspect");
        CHECK(ev.exception_name == "ACCESS_VIOLATION");
    }

    // CR-U3: be.log has NO "minidump:" line -> no throw, fields stay empty.
    {
        fs::path d = scratch("u3");
        write_file(d / "be.log",
                   "[backend] booting\n[backend] healthy (port up)\n[backend] shutting down\n");
        SafeStateEvent ev;
        CHECK_NOTHROW(xi::enrich_from_crash_report((d / "be.log").string(), ev));
        CHECK(ev.report_path.empty());
        CHECK(ev.exception_name.empty());
        CHECK(ev.faulting_module.empty());
        CHECK(ev.last_phase.empty());
    }

    // CR-U3b: be.log itself missing -> no throw, everything empty.
    {
        fs::path d = scratch("u3b");
        SafeStateEvent ev;
        CHECK_NOTHROW(xi::enrich_from_crash_report((d / "does-not-exist.log").string(), ev));
        CHECK(ev.report_path.empty());
        CHECK(ev.exception_name.empty());
    }

    // CR-U4: sibling .json MISSING -> report_path set (so the operator can look),
    // other fields empty, no throw.
    {
        fs::path d = scratch("u4");
        fs::path dmp = d / "orphan.dmp";
        write_file(dmp, "x");  // .dmp exists, .json deliberately does NOT
        write_file(d / "be.log", "minidump: " + dmp.string() + "\n");
        SafeStateEvent ev;
        CHECK_NOTHROW(xi::enrich_from_crash_report((d / "be.log").string(), ev));
        CHECK(!ev.report_path.empty());
        CHECK(fs::path(ev.report_path).extension() == ".json");
        CHECK(ev.exception_name.empty());
        CHECK(ev.faulting_module.empty());
        CHECK(ev.last_phase.empty());
    }

    // CR-U4b: sibling .json CORRUPT (not valid JSON) -> report_path set, no throw,
    // forensic fields empty (cJSON_Parse returns null, parser bails cleanly).
    {
        fs::path d = scratch("u4b");
        fs::path dmp = d / "corrupt.dmp";
        write_file(dmp, "x");
        write_file(fs::path(dmp).replace_extension(".json"),
                   "{ this is not valid json :: <<>> ");
        write_file(d / "be.log", "minidump: " + dmp.string() + "\n");
        SafeStateEvent ev;
        CHECK_NOTHROW(xi::enrich_from_crash_report((d / "be.log").string(), ev));
        CHECK(!ev.report_path.empty());
        CHECK(ev.exception_name.empty());
        CHECK(ev.last_phase.empty());
    }

    // CR-U5: multiple "minidump:" lines -> LAST wins (most recent crash). The
    // first names a stale report (WRONG_OLD); the parser must read the second.
    {
        fs::path d = scratch("u5");
        fs::path old_dmp = d / "old.dmp";
        fs::path new_dmp = d / "new.dmp";
        write_file(old_dmp, "x");
        write_file(new_dmp, "x");
        write_file(fs::path(old_dmp).replace_extension(".json"),
                   R"({"exception":{"name":"WRONG_OLD","module":"stale.dll"}})");
        write_file(fs::path(new_dmp).replace_extension(".json"),
                   R"({"exception":{"name":"ACCESS_VIOLATION","module":"plugin_v0.dll"},)"
                   R"("context":{"last_phase":"inspect"}})");
        write_file(d / "be.log",
                   "first run — minidump: " + old_dmp.string() + "\n"
                   "respawn, crashed again — minidump: " + new_dmp.string() + "\n");
        SafeStateEvent ev;
        CHECK_NOTHROW(xi::enrich_from_crash_report((d / "be.log").string(), ev));
        CHECK(ev.exception_name == "ACCESS_VIOLATION");   // not WRONG_OLD
        CHECK(ev.faulting_module == "plugin_v0.dll");     // not stale.dll
        CHECK(ev.last_phase == "inspect");
        CHECK(fs::path(ev.report_path).stem() == "new");  // resolved to the LAST dmp
    }

    // CR-U6 (edge/security): a "minidump:" path containing spaces + an embedded
    // ".dmp" earlier in the string. Non-greedy (.+?\.dmp) stops at the FIRST
    // ".dmp"; we just require no throw and that report_path is the .json sibling.
    {
        fs::path d = scratch("u6");
        fs::path dmp = d / "weird name.dmp";
        write_file(dmp, "x");
        write_file(fs::path(dmp).replace_extension(".json"),
                   R"({"exception":{"name":"ACCESS_VIOLATION"}})");
        write_file(d / "be.log", "minidump: " + dmp.string() + "  \r\n");  // trailing ws/CR
        SafeStateEvent ev;
        CHECK_NOTHROW(xi::enrich_from_crash_report((d / "be.log").string(), ev));
        CHECK(!ev.report_path.empty());
        CHECK(fs::path(ev.report_path).extension() == ".json");
    }

    // Cleanup scratch dirs (best-effort).
    {
        std::error_code ec;
        for (const char* t : {"u1","u2","u3","u3b","u4","u4b","u5","u6"})
            fs::remove_all(fs::temp_directory_path() / ("xinsp2-qa-edge-" + std::string(t)), ec);
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(s) FAILED\n", g_failures);
    return 1;
}
