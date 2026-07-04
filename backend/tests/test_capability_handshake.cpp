//
// test_capability_handshake.cpp — the LV2-style required/optional capability
// handshake (core_fix_plan.md §11 LV2 row / §12 Phase 3).
//
// Two layers, both exercised here:
//   1. PARSE — plugin.json `"requires":[{"iface","min"}]` + `"optional":[...]`
//      land in PluginInfo.required_ifaces / .optional_ifaces (xi_pm_parse.hpp).
//      Top-level only, `min` defaults to 1, malformed entries skipped.
//   2. GATE — plugin_caps_compatible(pi, host, …) (xi_cabi_adapter.hpp) refuses a
//      plugin whose REQUIRED interface the host's get_interface door does not
//      publish at a version >= min, surfacing a reason through the SAME err_msg
//      channel as the ABI gate (plugin_abi_compatible). OPTIONAL interfaces never
//      gate — the plugin null-checks them at runtime.
//
// Host published interfaces (ImagePool::make_host_api, all @1): xi.imaging@1,
// xi.doc@1, xi.emit@1, xi.log@1, xi.preview@1. (xi.legacy@9 retired in Phase 4.)
//
#include <xi/xi_cabi_adapter.hpp>   // PluginInfo / IfaceReq / plugin_caps_compatible
#include <xi/xi_pm_parse.hpp>       // parse_manifest / parse_iface_reqs
#include <xi/xi_image_pool.hpp>     // ImagePool::make_host_api (the get_interface door)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

// ---------------------------------------------------------------------------
// (1) Parse: requires[]/optional[] land in PluginInfo, top-level + min default.
// ---------------------------------------------------------------------------
static void test_parse_iface_reqs() {
    SECTION("parse_iface_reqs: requires[]/optional[], min default, malformed skip");

    // Two requires (one with explicit min, one defaulting to 1) + one optional.
    const std::string body =
        "{\n"
        "  \"name\": \"caps_demo\",\n"
        "  \"requires\": [ {\"iface\":\"xi.imaging\",\"min\":1},\n"
        "                  {\"iface\":\"xi.doc\"} ],\n"
        "  \"optional\": [ {\"iface\":\"xi.preview\",\"min\":1} ]\n"
        "}";

    auto req = xi::parse_iface_reqs(body, "requires");
    CHECK(req.size() == 2);
    if (req.size() == 2) {
        CHECK(req[0].iface == "xi.imaging");
        CHECK(req[0].min == 1);
        CHECK(req[1].iface == "xi.doc");
        CHECK(req[1].min == 1);          // absent min -> default 1
    }
    auto opt = xi::parse_iface_reqs(body, "optional");
    CHECK(opt.size() == 1);
    if (opt.size() == 1) {
        CHECK(opt[0].iface == "xi.preview");
        CHECK(opt[0].min == 1);
    }

    // Explicit higher min is preserved.
    auto v2 = xi::parse_iface_reqs(
        "{\"requires\":[{\"iface\":\"xi.imaging\",\"min\":2}]}", "requires");
    CHECK(v2.size() == 1 && v2[0].min == 2);

    // Missing key -> empty (the common case: no declared capabilities).
    CHECK(xi::parse_iface_reqs("{\"name\":\"x\"}", "requires").empty());

    // Malformed entries (no iface / blank iface / non-object) are skipped, the
    // good one survives.
    auto mixed = xi::parse_iface_reqs(
        "{\"requires\":[ {\"min\":1}, {\"iface\":\"\"}, 42, {\"iface\":\"xi.log\"} ]}",
        "requires");
    CHECK(mixed.size() == 1);
    if (mixed.size() == 1) CHECK(mixed[0].iface == "xi.log");

    // A requires buried in a nested manifest block must NOT be honoured (top-level
    // only — same hardening as json_flag_true / extract_string).
    const std::string nested =
        "{\"name\":\"x\",\"manifest\":{\"requires\":[{\"iface\":\"xi.imaging\"}]}}";
    CHECK(xi::parse_iface_reqs(nested, "requires").empty());
}

// parse_manifest end-to-end: the arrays reach PluginInfo from a real plugin.json.
static void test_parse_manifest_caps() {
    SECTION("parse_manifest: requires[]/optional[] reach PluginInfo");
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "xi_caps_handshake_test";
    fs::create_directories(dir);
    {
        std::ofstream f((dir / "plugin.json").string());
        f << "{\n"
             "  \"name\": \"caps_demo\",\n"
             "  \"requires\": [ {\"iface\":\"xi.imaging\",\"min\":1} ],\n"
             "  \"optional\": [ {\"iface\":\"xi.nope\",\"min\":1} ]\n"
             "}";
    }
    auto pi = xi::parse_manifest((dir / "plugin.json").string(), dir.string());
    CHECK(pi.name == "caps_demo");
    CHECK(pi.required_ifaces.size() == 1);
    CHECK(pi.optional_ifaces.size() == 1);
    if (pi.required_ifaces.size() == 1) CHECK(pi.required_ifaces[0].iface == "xi.imaging");
    if (pi.optional_ifaces.size() == 1) CHECK(pi.optional_ifaces[0].iface == "xi.nope");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// (2) Gate: plugin_caps_compatible against the real get_interface door.
// ---------------------------------------------------------------------------
static void test_caps_gate() {
    SECTION("plugin_caps_compatible: required satisfied loads, missing/too-old refuses");

    xi_host_api host = xi::ImagePool::make_host_api();
    CHECK(host.get_interface != nullptr);

    // host_publishes_iface honours ">= min" via the exact-match door.
    CHECK(xi::host_publishes_iface(&host, "xi.imaging", 1));
    CHECK(!xi::host_publishes_iface(&host, "xi.doc", 1));     // xi.doc retired at THE CUT (v12)
    CHECK(!xi::host_publishes_iface(&host, "xi.legacy", 9));  // xi.legacy retired in Phase 4
    CHECK(!xi::host_publishes_iface(&host, "xi.imaging", 2)); // host only publishes @1
    CHECK(!xi::host_publishes_iface(&host, "xi.nope", 1));    // unknown id

    // A plugin requiring xi.imaging@1 — the host publishes it → LOADS, no reason.
    {
        xi::PluginInfo pi;
        pi.name = "needs_imaging";
        pi.required_ifaces = { {"xi.imaging", 1} };
        std::string err = "sentinel";
        CHECK(xi::plugin_caps_compatible(pi, &host, pi.name, &err));
        CHECK(err == "sentinel");        // untouched on success
    }

    // A plugin requiring a NONEXISTENT iface — cleanly REFUSED with a reason that
    // names the missing capability.
    {
        xi::PluginInfo pi;
        pi.name = "needs_ghost";
        pi.required_ifaces = { {"xi.imaging", 1}, {"xi.ghost", 1} };
        std::string err;
        CHECK(!xi::plugin_caps_compatible(pi, &host, pi.name, &err));
        CHECK(!err.empty());
        CHECK(err.find("xi.ghost") != std::string::npos);
        CHECK(err.find("needs_ghost") != std::string::npos);
    }

    // A plugin requiring a min ABOVE the published version — REFUSED with a reason.
    {
        xi::PluginInfo pi;
        pi.name = "needs_imaging_v2";
        pi.required_ifaces = { {"xi.imaging", 2} };
        std::string err;
        CHECK(!xi::plugin_caps_compatible(pi, &host, pi.name, &err));
        CHECK(!err.empty());
        CHECK(err.find("xi.imaging@2") != std::string::npos);
    }

    // An OPTIONAL missing iface does NOT block load — only required[] gates.
    {
        xi::PluginInfo pi;
        pi.name = "opt_only";
        pi.optional_ifaces = { {"xi.ghost", 1} };   // missing, but optional
        std::string err;
        CHECK(xi::plugin_caps_compatible(pi, &host, pi.name, &err));
    }

    // No declared capabilities (the overwhelmingly common case) — always loads.
    {
        xi::PluginInfo pi;
        pi.name = "plain";
        std::string err;
        CHECK(xi::plugin_caps_compatible(pi, &host, pi.name, &err));
    }

    // A simulated pre-v10 host (no get_interface door) cannot satisfy ANY required
    // interface → refuse. (A plugin with no requires still loads — see "plain".)
    {
        xi_host_api old_host = host;
        old_host.get_interface = nullptr;
        xi::PluginInfo pi;
        pi.name = "needs_on_old_host";
        pi.required_ifaces = { {"xi.imaging", 1} };
        std::string err;
        CHECK(!xi::plugin_caps_compatible(pi, &old_host, pi.name, &err));
        CHECK(!err.empty());

        xi::PluginInfo plain;
        plain.name = "plain_on_old_host";
        std::string e2;
        CHECK(xi::plugin_caps_compatible(plain, &old_host, plain.name, &e2));
    }
}

int main() {
    std::printf("[test] LV2-style capability handshake (requires[]/optional[])\n");
    test_parse_iface_reqs();
    test_parse_manifest_caps();
    test_caps_gate();
    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
