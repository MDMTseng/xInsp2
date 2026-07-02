//
// test_omp_guard.cpp — the blessed-concurrency compile guard (adoption map item
// 11 / concurrency-review finding 4). A raw `#pragma omp` in an inspection
// SCRIPT's own source must be rejected at the JIT compile boundary, before
// cl.exe runs, with a diagnostic routing the author to xi::parallel_for /
// xi::async. Plugins are out of scope; an explicit project.json "allow_raw_omp"
// opts out.
//
// Two layers:
//   1. raw_omp_pragma_lines() — the pure line scanner (positive + negative shapes).
//   2. compile() — the rejection is wired for CompileMode::Script only, gated by
//      allow_raw_omp, and fires BEFORE any toolchain work (so this test needs no
//      cl.exe / OpenCV — the guard returns early).
//

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <xi/xi_script_compiler.hpp>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

using xi::script::raw_omp_pragma_lines;

static bool has_xi9001(const xi::script::CompileResult& r) {
    for (auto& d : r.diagnostics) if (d.code == "XI9001") return true;
    return false;
}

static std::string write_temp(const std::string& name, const std::string& body) {
    auto p = std::filesystem::temp_directory_path() / "xinsp2_omp_guard_test";
    std::filesystem::create_directories(p);
    auto f = (p / name).string();
    std::ofstream o(f, std::ios::binary);
    o << body;
    o.close();
    return f;
}

int main() {
    // ---- 1. scanner: positive shapes ---------------------------------------
    {
        // Standard directive.
        auto h = raw_omp_pragma_lines("int x;\n#pragma omp parallel for\nfor(;;){}\n");
        CHECK(h.size() == 1);
        if (!h.empty()) CHECK(h[0] == 2);
    }
    {
        // Leading whitespace + spaced `# pragma  omp` variants both count.
        auto h = raw_omp_pragma_lines("    #pragma omp parallel\n#  pragma   omp  for\n");
        CHECK(h.size() == 2);
    }
    {
        // Bare `#pragma omp` (e.g. `#pragma omp barrier`) — still raw omp.
        auto h = raw_omp_pragma_lines("#pragma omp barrier\n");
        CHECK(h.size() == 1);
    }

    // ---- 2. scanner: negative shapes (no false positives) ------------------
    {
        // `#pragma once` is not omp.
        CHECK(raw_omp_pragma_lines("#pragma once\n").empty());
        // A different pragma family.
        CHECK(raw_omp_pragma_lines("#pragma warning(disable:4996)\n").empty());
        // "omp" as an identifier / inside a string on a code line.
        CHECK(raw_omp_pragma_lines("int omp = 3; const char* s = \"#pragma omp\";\n").empty());
        // A commented-out example must not be flagged.
        CHECK(raw_omp_pragma_lines("// #pragma omp parallel for\n").empty());
        CHECK(raw_omp_pragma_lines("int y; // #pragma omp parallel\n").empty());
        // `#pragma ompX` / `pragmaX` must not match on the token boundary.
        CHECK(raw_omp_pragma_lines("#pragma ompire\n").empty());
        CHECK(raw_omp_pragma_lines("#pragmaomp\n").empty());
        // Empty input.
        CHECK(raw_omp_pragma_lines("").empty());
    }

    // ---- 3. compile(): Script mode rejects a raw omp with XI9001 -----------
    {
        std::string src = write_temp(
            "inspect.cpp",
            "#include <xi/xi.hpp>\nvoid inspect(int){\n#pragma omp parallel for\n"
            "  for(int i=0;i<8;++i){}\n}\n");
        xi::script::CompileRequest req;
        req.source_path = src;
        req.output_dir  = (std::filesystem::temp_directory_path()
                           / "xinsp2_omp_guard_test" / "build").string();
        req.mode        = xi::script::CompileMode::Script;   // guarded
        auto r = xi::script::compile(req);
        CHECK(!r.ok);
        CHECK(has_xi9001(r));
        // Diagnostic points at the pragma line and routes to the blessed wrappers.
        bool routed = false;
        for (auto& d : r.diagnostics)
            if (d.code == "XI9001") {
                CHECK(d.line == 3);
                CHECK(d.severity == "error");
                routed = d.message.find("xi::parallel_for") != std::string::npos &&
                         d.message.find("xi::async") != std::string::npos;
            }
        CHECK(routed);
    }

    // ---- 4. compile(): a clean script is NOT flagged (guard fires no XI9001) -
    {
        std::string src = write_temp(
            "clean.cpp",
            "#include <xi/xi.hpp>\nvoid inspect(int){\n"
            "  xi::parallel_for(8, [&](int i){ (void)i; });\n}\n");
        xi::script::CompileRequest req;
        req.source_path = src;
        req.output_dir  = (std::filesystem::temp_directory_path()
                           / "xinsp2_omp_guard_test" / "build").string();
        req.mode        = xi::script::CompileMode::Script;
        auto r = xi::script::compile(req);
        // It may still fail later (no OpenCV/vcvars in this hermetic test), but it
        // must NOT be rejected by the omp guard.
        CHECK(!has_xi9001(r));
    }

    // ---- 5. allow_raw_omp opt-out bypasses the guard -----------------------
    {
        std::string src = write_temp(
            "optout.cpp",
            "#include <xi/xi.hpp>\nvoid inspect(int){\n#pragma omp parallel for\n"
            "  for(int i=0;i<8;++i){}\n}\n");
        xi::script::CompileRequest req;
        req.source_path   = src;
        req.output_dir    = (std::filesystem::temp_directory_path()
                             / "xinsp2_omp_guard_test" / "build").string();
        req.mode          = xi::script::CompileMode::Script;
        req.allow_raw_omp = true;   // author accepts the rules
        auto r = xi::script::compile(req);
        CHECK(!has_xi9001(r));      // guard skipped
    }

    // ---- 6. plugins are out of scope (guard only touches Script mode) ------
    {
        std::string src = write_temp(
            "plugin.cpp",
            "#pragma omp parallel for\nfor(int i=0;i<8;++i){}\n");
        xi::script::CompileRequest req;
        req.source_path = src;
        req.output_dir  = (std::filesystem::temp_directory_path()
                           / "xinsp2_omp_guard_test" / "build").string();
        req.mode        = xi::script::CompileMode::PluginDev;   // not guarded
        auto r = xi::script::compile(req);
        CHECK(!has_xi9001(r));
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_omp_guard: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_omp_guard: %d FAIL\n", g_failures);
    return 1;
}
