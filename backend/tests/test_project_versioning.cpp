//
// test_project_versioning.cpp — project-file version identity + full-document
// save (adoption item 12 / ext_review 06 Finding 1+2).
//
// Covers:
//   1. parse_project_schema — family/major parsing of "xi.project/N".
//   2. merge_project_json (the `save_project` command path) — read–modify–write:
//      unknown top-level keys survive a save verbatim, ONLY params/instances are
//      overwritten, and the schema is stamped. This is the round-trip honesty
//      test: seed unknown keys → mutate params/instances → everything else is
//      byte-preserved + schema added.
//   3. save_project_locked stamps the schema (via the public create_project).
//   4. The loader schema gate: a legacy (no-schema) file opens; the current
//      schema opens; an unrecognized FUTURE major / foreign family is REFUSED
//      with both versions named (the project-file analogue of the ABI gate).
//
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <yyjson.h>
#include <xi/xi_project.hpp>
#include <xi/xi_plugin_manager.hpp>

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

// ---- tiny yyjson read helpers (test-only assertions) ---------------------
static yyjson_val* obj_path(yyjson_val* root, std::initializer_list<const char*> path) {
    yyjson_val* v = root;
    for (const char* k : path) {
        if (!v || !yyjson_is_obj(v)) return nullptr;
        v = yyjson_obj_get(v, k);
    }
    return v;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static fs::path scratch(const std::string& name) {
    fs::path d = fs::temp_directory_path() / ("xi_projver_" + name);
    std::error_code ec; fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

// ==========================================================================
static void test_parse_schema() {
    SECTION("parse_project_schema: family + major");
    using xi::project::parse_project_schema;
    int m = -1;
    CHECK(parse_project_schema("xi.project/1", m) && m == 1);
    CHECK(parse_project_schema("xi.project/2", m) && m == 2);
    CHECK(parse_project_schema("xi.project/10", m) && m == 10);
    // Foreign family / malformed / missing major → not recognized.
    m = -1;
    CHECK(!parse_project_schema("xi.run-outcome/1", m));
    CHECK(!parse_project_schema("xi.project", m));
    CHECK(!parse_project_schema("xi.project/", m));
    CHECK(!parse_project_schema("xi.project/x", m));
    CHECK(!parse_project_schema("xi.project/1x", m));
    CHECK(!parse_project_schema("", m));
    // The current constant round-trips through the parser.
    CHECK(parse_project_schema(xi::project::kProjectSchema, m) &&
          m == xi::project::kProjectSchemaMajor);
}

// ==========================================================================
static void test_merge_preserves_unknown_stamps_schema() {
    SECTION("merge_project_json: unknown top keys survive; only params/instances change; schema stamped");
    // A realistic multi-writer project.json with NO schema field yet, carrying
    // backend-owned keys AND foreign keys (the VS Code extension's), plus an
    // unknown nested key inside a preserved (non-owned) block.
    const std::string existing = R"({
      "name": "legacy_proj",
      "params": [ {"name":"sigma","value":1.0} ],
      "instances": [ {"name":"old_inst"} ],
      "runtime": { "timer_fps": 5, "process_priority": "high", "future_nested": true },
      "parallelism": { "dispatch_threads": 4 },
      "groups": [ {"name":"g0"} ],
      "plugin_dirs": [ "./plugins" ],
      "auto_respawn": true,
      "watchdog_ms": 2000,
      "some_future_top_key": "keep-me"
    })";
    const std::string new_params    = R"([ {"name":"sigma","value":9.9} ])";
    const std::string new_instances = R"([ {"name":"new_inst","plugin":"P"} ])";

    std::string out = xi::project::merge_project_json(existing, new_params, new_instances);

    yyjson_doc* d = yyjson_read(out.data(), out.size(), 0);
    CHECK(d != nullptr);
    yyjson_val* root = d ? yyjson_doc_get_root(d) : nullptr;
    CHECK(root && yyjson_is_obj(root));

    // schema stamped.
    yyjson_val* sch = obj_path(root, {"schema"});
    CHECK(sch && yyjson_is_str(sch) &&
          std::string(yyjson_get_str(sch)) == xi::project::kProjectSchema);

    // OWNED keys took the new values (mutated).
    yyjson_val* params = obj_path(root, {"params"});
    CHECK(params && yyjson_is_arr(params) && yyjson_arr_size(params) == 1);
    yyjson_val* sigma = obj_path(yyjson_arr_get(params, 0), {"value"});
    CHECK(sigma && yyjson_get_real(sigma) > 9.0);   // 9.9, the mutated value
    yyjson_val* insts = obj_path(root, {"instances"});
    CHECK(insts && yyjson_is_arr(insts) && yyjson_arr_size(insts) == 1);
    yyjson_val* iname = obj_path(yyjson_arr_get(insts, 0), {"name"});
    CHECK(iname && std::string(yyjson_get_str(iname)) == "new_inst");

    // UNKNOWN / foreign top-level keys preserved verbatim.
    yyjson_val* ar = obj_path(root, {"auto_respawn"});
    CHECK(ar && yyjson_is_true(ar));
    yyjson_val* wd = obj_path(root, {"watchdog_ms"});
    CHECK(wd && yyjson_get_int(wd) == 2000);
    yyjson_val* fk = obj_path(root, {"some_future_top_key"});
    CHECK(fk && std::string(yyjson_get_str(fk)) == "keep-me");

    // Backend-owned-but-not-emitted-by-this-saver keys ALSO preserved (the
    // save_project command doesn't own runtime/parallelism/groups; the review's
    // core data-loss finding was exactly these being dropped).
    yyjson_val* rt_fps = obj_path(root, {"runtime", "timer_fps"});
    CHECK(rt_fps && yyjson_get_int(rt_fps) == 5);
    yyjson_val* rt_nested = obj_path(root, {"runtime", "future_nested"});
    CHECK(rt_nested && yyjson_is_true(rt_nested));   // nested unknown survives (whole subtree copied)
    yyjson_val* par = obj_path(root, {"parallelism", "dispatch_threads"});
    CHECK(par && yyjson_get_int(par) == 4);
    yyjson_val* groups = obj_path(root, {"groups"});
    CHECK(groups && yyjson_is_arr(groups) && yyjson_arr_size(groups) == 1);
    yyjson_val* pdirs = obj_path(root, {"plugin_dirs"});
    CHECK(pdirs && yyjson_is_arr(pdirs));

    // The old params/instances values must NOT linger — no duplicate keys, and
    // the sigma value is the NEW one (checked above), proving new-wins.
    if (d) yyjson_doc_free(d);
}

static void test_merge_absent_corrupt_and_schema_replace() {
    SECTION("merge_project_json: absent/corrupt existing → clean doc; existing schema replaced");
    // Absent existing → still a well-formed doc with schema+params+instances.
    {
        std::string out = xi::project::merge_project_json("", "[]", "[]");
        yyjson_doc* d = yyjson_read(out.data(), out.size(), 0);
        yyjson_val* root = d ? yyjson_doc_get_root(d) : nullptr;
        CHECK(root && yyjson_is_obj(root));
        CHECK(obj_path(root, {"schema"}) && obj_path(root, {"params"}) &&
              obj_path(root, {"instances"}));
        if (d) yyjson_doc_free(d);
    }
    // Corrupt existing → treated as absent (no junk carried through), no crash.
    {
        std::string out = xi::project::merge_project_json("{ not json at all", "[]", "[]");
        yyjson_doc* d = yyjson_read(out.data(), out.size(), 0);
        CHECK(d != nullptr);   // output is still valid JSON
        yyjson_val* root = d ? yyjson_doc_get_root(d) : nullptr;
        CHECK(root && obj_path(root, {"schema"}));
        if (d) yyjson_doc_free(d);
    }
    // Existing carries a DIFFERENT schema → the current one wins, exactly once.
    {
        const std::string existing = R"({"schema":"xi.project/99","keep":"me"})";
        std::string out = xi::project::merge_project_json(existing, "[]", "[]");
        yyjson_doc* d = yyjson_read(out.data(), out.size(), 0);
        yyjson_val* root = d ? yyjson_doc_get_root(d) : nullptr;
        yyjson_val* sch = root ? yyjson_obj_get(root, "schema") : nullptr;
        CHECK(sch && std::string(yyjson_get_str(sch)) == xi::project::kProjectSchema);
        CHECK(obj_path(root, {"keep"}));   // unrelated key still preserved
        if (d) yyjson_doc_free(d);
    }
}

// ==========================================================================
static void test_create_project_stamps_schema() {
    SECTION("save_project_locked stamps schema (via create_project)");
    fs::path root = scratch("create");
    xi::PluginManager pm;
    CHECK(pm.create_project(root.string(), "MadeHere"));
    std::string pj = read_file(root / "project.json");
    CHECK(!pj.empty());
    yyjson_doc* d = yyjson_read(pj.data(), pj.size(), 0);
    yyjson_val* r = d ? yyjson_doc_get_root(d) : nullptr;
    yyjson_val* sch = r ? yyjson_obj_get(r, "schema") : nullptr;
    CHECK(sch && yyjson_is_str(sch) &&
          std::string(yyjson_get_str(sch)) == xi::project::kProjectSchema);
    if (d) yyjson_doc_free(d);
    std::error_code ec; fs::remove_all(root, ec);
}

// ==========================================================================
static void write_project_json(const fs::path& folder, const std::string& body) {
    std::error_code ec; fs::create_directories(folder, ec);
    std::ofstream f((folder / "project.json").string(), std::ios::binary);
    f << body;
}

static void test_loader_gate() {
    SECTION("open_project schema gate: legacy accepted, current accepted, future/foreign refused");

    // (a) Legacy file (no schema) — accepted, no hard-refusal error.
    {
        fs::path root = scratch("legacy");
        write_project_json(root, R"({"name":"legacy"})");
        xi::PluginManager pm;
        // QuiesceToken: bare PluginManager, single-threaded test — no dispatch pool.
        CHECK(pm.open_project(xi::QuiesceToken::assert_no_dispatch(), root.string(), /*working_copy=*/false));
        CHECK(pm.open_error().empty());
        std::error_code ec; fs::remove_all(root, ec);
    }

    // (b) Current schema — accepted.
    {
        fs::path root = scratch("current");
        write_project_json(root, R"({"schema":"xi.project/1","name":"cur"})");
        xi::PluginManager pm;
        CHECK(pm.open_project(xi::QuiesceToken::assert_no_dispatch(), root.string(), false));
        CHECK(pm.open_error().empty());
        std::error_code ec; fs::remove_all(root, ec);
    }

    // (c) Future major — REFUSED, both versions named.
    {
        fs::path root = scratch("future");
        write_project_json(root, R"({"schema":"xi.project/2","name":"fut"})");
        xi::PluginManager pm;
        CHECK(!pm.open_project(xi::QuiesceToken::assert_no_dispatch(), root.string(), false));   // hard refusal
        std::string oe = pm.open_error();
        CHECK(oe.find("xi.project/2") != std::string::npos);   // the file's version
        CHECK(oe.find("xi.project/1") != std::string::npos);   // the backend's version
        std::error_code ec; fs::remove_all(root, ec);
    }

    // (d) Foreign family — REFUSED (unrecognized), file's string named.
    {
        fs::path root = scratch("foreign");
        write_project_json(root, R"({"schema":"acme.project/1","name":"x"})");
        xi::PluginManager pm;
        CHECK(!pm.open_project(xi::QuiesceToken::assert_no_dispatch(), root.string(), false));
        CHECK(pm.open_error().find("acme.project/1") != std::string::npos);
        std::error_code ec; fs::remove_all(root, ec);
    }

    // (e) A refused open must not leave a stale error on a subsequent good open.
    {
        fs::path bad = scratch("stale_bad");
        write_project_json(bad, R"({"schema":"xi.project/9"})");
        fs::path good = scratch("stale_good");
        write_project_json(good, R"({"schema":"xi.project/1","name":"ok"})");
        xi::PluginManager pm;
        CHECK(!pm.open_project(xi::QuiesceToken::assert_no_dispatch(), bad.string(), false));
        CHECK(!pm.open_error().empty());
        CHECK(pm.open_project(xi::QuiesceToken::assert_no_dispatch(), good.string(), false));
        CHECK(pm.open_error().empty());   // cleared on the good open
        std::error_code ec; fs::remove_all(bad, ec); fs::remove_all(good, ec);
    }
}

// H2: a project.json `instances` array is otherwise INERT — instances only
// materialize from instances/<name>/instance.json. A declared entry with NO
// backing dir used to vanish with no signal (qa_multi_graph: a project.json-only
// `expose` silently didn't exist). The loader now warns loudly, naming each
// phantom, while a declared entry that DOES have a backing dir is not falsely
// flagged as inert.
static void test_phantom_instance_warns() {
    SECTION("H2: project.json instance with no backing dir emits a loud warning");
    fs::path root = scratch("phantom_inst");

    // project.json declares two instances: `backed` (has a dir) and `phantom`
    // (no dir at all — the inert case). `plugin` names are irrelevant to the
    // phantom check (it fires purely on the missing backing file).
    write_project_json(root, R"({
      "schema":"xi.project/1",
      "name":"phantom_inst",
      "instances":[
        {"name":"backed",  "plugin":"mock_camera"},
        {"name":"phantom", "plugin":"expose"}
      ]
    })");
    // Give `backed` a real instances/backed/instance.json so it is NOT a phantom
    // (it may still fail to load for lack of the plugin, but that's a different,
    // non-phantom warning keyed off the present backing file).
    fs::create_directories(root / "instances" / "backed");
    { std::ofstream f((root / "instances" / "backed" / "instance.json").string(), std::ios::binary);
      f << R"({"plugin":"mock_camera"})"; }

    xi::PluginManager pm;
    CHECK(pm.open_project(xi::QuiesceToken::assert_no_dispatch(), root.string(), /*working_copy=*/false));   // load succeeds (warnings, not errors)
    auto ws = pm.open_warnings();

    bool phantom_warned = false;
    bool backed_flagged_inert = false;
    for (auto& w : ws) {
        const bool inert = w.reason.find("INERT") != std::string::npos ||
                           w.reason.find("project.json 'instances'") != std::string::npos;
        if (w.instance == "phantom" && inert) phantom_warned = true;
        if (w.instance == "backed"  && inert) backed_flagged_inert = true;
    }
    CHECK(phantom_warned);          // the phantom is named and flagged inert
    CHECK(!backed_flagged_inert);   // a backed entry is never mis-flagged as inert

    std::error_code ec; fs::remove_all(root, ec);
}

int main() {
    test_parse_schema();
    test_merge_preserves_unknown_stamps_schema();
    test_merge_absent_corrupt_and_schema_replace();
    test_create_project_stamps_schema();
    test_loader_gate();
    test_phantom_instance_warns();
    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
