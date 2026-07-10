//
// test_plugin_teardown.cpp — in-process e2e for the plugin-teardown bugs
// (#13 manifest-renamed plugin + #9 compile:false external). Drives a real
// PluginManager through open_project / load_plugin / close_project against a
// temp project that uses a real (prebuilt) xi C-ABI plugin DLL whose MANIFEST
// name differs from its FOLDER leaf, plus a compile:false external.
//
// What it proves: after close_project, EVERY plugin the project loaded is freed
// AND erased from the registry — including the manifest-renamed one (whose
// HMODULE the old folder-keyed teardown missed) and the compile:false external
// (which the old origin-only teardown never freed at all). With the bug, the
// stale entry survives (find_plugin still returns it) and its HMODULE leaks.
//
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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

static void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p.string(), std::ios::binary);
    f << body;
}

static void copy_dll(const fs::path& src, const fs::path& dst, std::error_code& ec) {
    fs::create_directories(dst.parent_path());
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
}

int main() {
    std::printf("[test] plugin teardown frees manifest-renamed + external plugins (#13/#9)\n");

    const fs::path dll_src = TEST_PLUGIN_DLL;   // built test_teardown_plugin DLL
    if (!fs::exists(dll_src)) {
        std::fprintf(stderr, "FAIL: test plugin DLL missing: %s\n", dll_src.string().c_str());
        return 1;
    }

    fs::path proj = fs::temp_directory_path() / "xi_teardown_test_proj";
    std::error_code ec;
    fs::remove_all(proj, ec);

    // project.json: declarative model. One compile:true project plugin whose
    // manifest name (renamed_proj) != folder leaf (folderleaf_proj) — exercises
    // #13. One compile:false external (renamed_ext) — exercises #9.
    write_file(proj / "project.json",
        "{\n"
        "  \"name\": \"teardown_test\",\n"
        "  \"plugin_dirs\": [\"./plugins\"],\n"
        "  \"plugins\": {\n"
        "    \"p\": { \"path\": \"folderleaf_proj\", \"compile\": true },\n"
        "    \"e\": { \"path\": \"folderleaf_ext\",  \"compile\": false }\n"
        "  }\n"
        "}\n");

    // compile:true, build:cmake (prebuilt) — manifest name differs from folder.
    write_file(proj / "plugins" / "folderleaf_proj" / "plugin.json",
        "{ \"name\": \"renamed_proj\", \"build\": \"cmake\", \"dll\": \"renamed_proj.dll\" }\n");
    copy_dll(dll_src, proj / "plugins" / "folderleaf_proj" / "build" / "renamed_proj.dll", ec);
    CHECK(!ec);

    // compile:false external, build:cmake (prebuilt). load_plugin resolves the DLL
    // next to plugin.json (folder_path/dll_name).
    write_file(proj / "plugins" / "folderleaf_ext" / "plugin.json",
        "{ \"name\": \"renamed_ext\", \"build\": \"cmake\", \"dll\": \"renamed_ext.dll\" }\n");
    copy_dll(dll_src, proj / "plugins" / "folderleaf_ext" / "renamed_ext.dll", ec);
    CHECK(!ec);

    {
        xi::PluginManager mgr;
        // QuiesceToken: bare PluginManager, single-threaded test — no dispatch pool.
        CHECK(mgr.open_project(xi::QuiesceToken::assert_no_dispatch(), proj.string()));

        // compile:true project plugin is loaded eagerly (handle non-null), keyed by
        // its MANIFEST name.
        auto* pp = mgr.find_plugin("renamed_proj");
        CHECK(pp != nullptr);
        CHECK(pp && pp->handle != nullptr);
        CHECK(mgr.is_project_plugin("renamed_proj"));   // origin keyed by manifest name

        // compile:false external is registered (handle null until load_plugin).
        auto* pe = mgr.find_plugin("renamed_ext");
        CHECK(pe != nullptr);
        std::string lerr;
        CHECK(mgr.load_plugin("renamed_ext", &lerr));   // now its HMODULE is live
        pe = mgr.find_plugin("renamed_ext");
        CHECK(pe != nullptr);
        CHECK(pe && pe->handle != nullptr);

        // Teardown: close_project must free + erase EVERY plugin this project
        // loaded, by the key that holds the handle.
        mgr.close_project(xi::QuiesceToken::assert_no_dispatch());

        CHECK(mgr.find_plugin("renamed_proj") == nullptr);   // #13: renamed handle freed+erased
        CHECK(mgr.find_plugin("renamed_ext")  == nullptr);   // #9 : external freed+erased
    }

    fs::remove_all(proj, ec);

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
