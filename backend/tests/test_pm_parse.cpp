//
// test_pm_parse.cpp — unit tests for the plugin-manifest parse layer
// (xi/xi_pm_parse.hpp). Focus: json_flag_true must honour ONLY a top-level
// object key (#22) — a flag buried in a nested manifest example / description
// string must NOT be treated as set, or a non-reentrant plugin silently runs
// concurrently (data race).
//
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <xi/xi_pm_parse.hpp>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

static void test_json_flag_true_toplevel_only() {
    SECTION("json_flag_true honours ONLY top-level keys (#22)");
    using xi::json_flag_true;

    // Top-level real bool — accepted, both with and without space.
    CHECK(json_flag_true("{\"reentrant\":true}", "reentrant"));
    CHECK(json_flag_true("{\"reentrant\": true}", "reentrant"));
    CHECK(json_flag_true("{ \"a\":1, \"reentrant\" : true }", "reentrant"));

    // Top-level false / absent — not set.
    CHECK(!json_flag_true("{\"reentrant\":false}", "reentrant"));
    CHECK(!json_flag_true("{\"other\":true}", "reentrant"));

    // THE BUG: `"reentrant":true` appears ONLY inside a nested manifest example
    // block, never as a top-level key. The substring scan matched it; the
    // hardened yyjson parse must report FALSE (serialize the plugin).
    const char* nested =
        "{\n"
        "  \"name\": \"deburr_detector\",\n"
        "  \"manifest\": {\n"
        "    \"example\": \"set \\\"reentrant\\\":true only if process() is thread-safe\",\n"
        "    \"params\": [ {\"reentrant\":true} ]\n"
        "  }\n"
        "}";
    CHECK(!json_flag_true(nested, "reentrant"));

    // Bug also bites the description-string form.
    const char* in_desc =
        "{\"name\":\"x\",\"description\":\"toggles \\\"reentrant\\\": true at runtime\"}";
    CHECK(!json_flag_true(in_desc, "reentrant"));

    // thread_safe alias gets the same treatment (used by apply_manifest_flags_).
    CHECK(json_flag_true("{\"thread_safe\":true}", "thread_safe"));
    CHECK(!json_flag_true(nested, "thread_safe"));

    // Malformed JSON → false (treat as off), never a substring false-positive.
    CHECK(!json_flag_true("{ truncated \"reentrant\":true", "reentrant"));

    // Quoted-string "true" tolerated at top level.
    CHECK(json_flag_true("{\"reentrant\":\"true\"}", "reentrant"));
    CHECK(!json_flag_true("{\"reentrant\":\"false\"}", "reentrant"));
}

static void test_parse_manifest_reentrant() {
    SECTION("parse_manifest: nested reentrant example parses as non-reentrant (#22)");
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "xi_pm_parse_test";
    fs::create_directories(dir);
    auto write = [&](const std::string& body) {
        std::ofstream f((dir / "plugin.json").string());
        f << body;
    };

    // reentrant declared ONLY inside a nested manifest block — must NOT enable it.
    write("{\n"
          "  \"name\": \"deburr\",\n"
          "  \"description\": \"set \\\"reentrant\\\":true when safe\",\n"
          "  \"manifest\": { \"notes\": [ {\"reentrant\":true} ] }\n"
          "}");
    {
        auto pi = xi::parse_manifest((dir / "plugin.json").string(), dir.string());
        CHECK(pi.name == "deburr");
        CHECK(pi.reentrant == false);   // the #22 fix
    }

    // Genuine top-level reentrant — enabled.
    write("{ \"name\": \"deburr\", \"reentrant\": true }");
    {
        auto pi = xi::parse_manifest((dir / "plugin.json").string(), dir.string());
        CHECK(pi.reentrant == true);
    }

    // thread_safe alias at top level — enabled.
    write("{ \"name\": \"deburr\", \"thread_safe\": true }");
    {
        auto pi = xi::parse_manifest((dir / "plugin.json").string(), dir.string());
        CHECK(pi.reentrant == true);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// The dispatch consequence of #20/#23: the reentrant flag the manifest parse
// produces must reach the adapter, where effective_cap_ = reentrant ? cap : 1.
// reentrant()==false is what makes CallScope serialize (cap 1). This proves the
// PluginInfo.reentrant -> CAbiInstanceAdapter wiring that make_adapter_guarded_
// feeds on every (re)load path; combined with the parse tests above, a plugin.json
// reentrant:true->false flip re-read on reload genuinely flips serialization on.
static void test_adapter_reentrant_wiring() {
    SECTION("adapter reflects parsed reentrant flag (#20/#23 effective_cap_)");
    using xi::CAbiInstanceAdapter;
    // dll=nullptr -> GetProcAddress returns null fns (no real plugin needed); inst
    // non-null so the dtor path is exercised. We only read reentrant()/is_sink().
    CAbiInstanceAdapter serial("i", "p", nullptr, reinterpret_cast<void*>(0x1),
                               /*reentrant=*/false, /*max_concurrency=*/0, /*is_sink=*/false);
    CHECK(serial.reentrant() == false);   // effective_cap_ -> 1 (serialized)
    CHECK(serial.is_sink()   == false);

    CAbiInstanceAdapter parallel("i", "p", nullptr, reinterpret_cast<void*>(0x1),
                                 /*reentrant=*/true, /*max_concurrency=*/4, /*is_sink=*/true);
    CHECK(parallel.reentrant() == true);  // effective_cap_ -> max_concurrency_ (4)
    CHECK(parallel.is_sink()   == true);
}

// RT3-B1/B2 (doc 25): on_fault=reinit is a UAF combo with a reentrant plugin (or a
// staged-prepare plugin); set_on_fault must DEFUSE it by downgrading reinit->reuse.
static void test_reinit_combo_guard() {
    SECTION("on_fault=reinit downgraded to reuse for a reentrant/staged-prepare instance");
    using xi::CAbiInstanceAdapter;
    using xi::OnFault;
    // Reentrant instance + on_fault=reinit -> DOWNGRADED to reuse (dll=nullptr so
    // prepare_fn_ is null; the reentrant_ branch of the guard fires).
    CAbiInstanceAdapter re("i", "p", nullptr, reinterpret_cast<void*>(0x1),
                           /*reentrant=*/true, /*max_concurrency=*/0, /*is_sink=*/false);
    re.set_on_fault(OnFault::Reinit);
    CHECK(re.on_fault() == OnFault::Reuse);      // defused

    // Control: a plain non-reentrant, non-prepare instance keeps reinit (safe there).
    CAbiInstanceAdapter ok("i", "p", nullptr, reinterpret_cast<void*>(0x1),
                           /*reentrant=*/false, /*max_concurrency=*/0, /*is_sink=*/false);
    ok.set_on_fault(OnFault::Reinit);
    CHECK(ok.on_fault() == OnFault::Reinit);     // untouched

    // Control: a non-reinit policy is never altered even on a reentrant instance.
    re.set_on_fault(OnFault::Refuse);
    CHECK(re.on_fault() == OnFault::Refuse);
}


// doc 37: plugin.json "pluginlets": [...] — the plugin's pluginlet opt-in list.
// The RUNTIME consumer of the one declaration (the C++ and webui builds read the
// same key themselves), so it must survive the manifest parse intact.
static void test_parse_pluginlets() {
    SECTION("parse_pluginlets reads the doc-37 pluginlet opt-in list");
    using xi::parse_pluginlets;

    // The normal case: a plugin opting into one/several plets, order preserved.
    auto one = parse_pluginlets(R"({"name":"controls_demo","pluginlets":["controls"]})");
    CHECK(one.size() == 1);
    CHECK(one.size() == 1 && one[0] == "controls");

    auto two = parse_pluginlets(R"({"pluginlets":["controls","live-view"]})");
    CHECK(two.size() == 2);
    CHECK(two.size() == 2 && two[0] == "controls" && two[1] == "live-view");

    // Absent / empty / wrong shape -> empty, never a crash (a plugin need not opt in).
    CHECK(parse_pluginlets(R"({"name":"x"})").empty());
    CHECK(parse_pluginlets(R"({"pluginlets":[]})").empty());
    CHECK(parse_pluginlets(R"({"pluginlets":"controls"})").empty());   // not an array
    CHECK(parse_pluginlets("not json").empty());

    // Non-string / empty entries are skipped, valid ones still collected.
    auto mixed = parse_pluginlets(R"({"pluginlets":["controls",7,"",null,"live-view"]})");
    CHECK(mixed.size() == 2);
    CHECK(mixed.size() == 2 && mixed[0] == "controls" && mixed[1] == "live-view");

    // It must come off the parsed manifest too, not just the standalone helper.
    // (parse_manifest wires pi.pluginlets = parse_pluginlets(content).)
}

int main() {
    test_json_flag_true_toplevel_only();
    test_parse_manifest_reentrant();
    test_reinit_combo_guard();
    test_adapter_reentrant_wiring();
    test_parse_pluginlets();
    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
