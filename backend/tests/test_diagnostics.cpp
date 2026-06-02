//
// test_diagnostics.cpp — unit test for xi::script::parse_diagnostics().
//
// Covers the cl.exe / link.exe output shapes the extension squiggle path
// has to deal with: error / warning / fatal error / linker error / no col
// / no line.
//

#include <cassert>
#include <cstdio>
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

int main() {
    using xi::script::parse_diagnostics;

    // 1. error with line+col
    {
        auto d = parse_diagnostics(
            "C:\\proj\\inspection.cpp(42,15): error C2065: 'undeclared_var': undeclared identifier\n");
        CHECK(d.size() == 1);
        if (!d.empty()) {
            CHECK(d[0].file == "C:\\proj\\inspection.cpp");
            CHECK(d[0].line == 42);
            CHECK(d[0].col == 15);
            CHECK(d[0].severity == "error");
            CHECK(d[0].code == "C2065");
            CHECK(d[0].message.find("undeclared identifier") != std::string::npos);
        }
    }

    // 2. warning with line only
    {
        auto d = parse_diagnostics(
            "foo.cpp(99): warning C4996: 'getenv': This function or variable may be unsafe.\n");
        CHECK(d.size() == 1);
        if (!d.empty()) {
            CHECK(d[0].file == "foo.cpp");
            CHECK(d[0].line == 99);
            CHECK(d[0].col == 0);
            CHECK(d[0].severity == "warning");
            CHECK(d[0].code == "C4996");
        }
    }

    // 3. fatal error
    {
        auto d = parse_diagnostics(
            "foo.cpp(10,1): fatal error C1083: Cannot open include file: 'missing.h'\n");
        CHECK(d.size() == 1);
        if (!d.empty()) {
            CHECK(d[0].severity == "error");
            CHECK(d[0].code == "C1083");
            CHECK(d[0].line == 10);
        }
    }

    // 4. linker error — no line/col, file is .obj
    {
        auto d = parse_diagnostics(
            "foo.obj : error LNK2019: unresolved external symbol bar referenced in main\n");
        CHECK(d.size() == 1);
        if (!d.empty()) {
            CHECK(d[0].severity == "error");
            CHECK(d[0].code == "LNK2019");
            CHECK(d[0].line == 0);
            CHECK(d[0].col == 0);
            CHECK(d[0].file.find("foo.obj") != std::string::npos);
        }
    }

    // 5. multiple lines, mix of severities, plus noise lines
    {
        std::string log =
            "Microsoft (R) C/C++ Optimizing Compiler Version blah\n"
            "inspection.cpp\n"
            "inspection.cpp(7,1): error C2143: syntax error: missing ';' before '}'\n"
            "inspection.cpp(8,5): warning C4101: 'x': unreferenced local variable\n"
            "Generating code\n"
            "inspection.cpp(15,9): note: see declaration of 'foo'\n";
        auto d = parse_diagnostics(log);
        CHECK(d.size() == 3);
        if (d.size() == 3) {
            CHECK(d[0].severity == "error");
            CHECK(d[0].line == 7);
            CHECK(d[1].severity == "warning");
            CHECK(d[1].line == 8);
            CHECK(d[2].severity == "note");
            CHECK(d[2].line == 15);
        }
    }

    // 6. empty input
    {
        auto d = parse_diagnostics("");
        CHECK(d.empty());
    }

    // ---- ensure_utf8: compiler diagnostics must NEVER be mojibake on the wire.
    // Regression for the friction the dogfood agent hit (cl.exe emits localized
    // diagnostics in the local code page; raw CP950/CP932 bytes on a UTF-8 frame
    // are garbage). The invariant ensure_utf8 guarantees, machine-independent:
    // the result is ALWAYS valid UTF-8 (whatever the host code page).
    {
        using xi::script::ensure_utf8;
        using xi::script::is_valid_utf8;

        // ASCII passes through unchanged.
        std::string ascii = "C:\\proj\\inspection.cpp(2): error C2059: syntax error: 'return'";
        CHECK(ensure_utf8(ascii) == ascii);
        CHECK(is_valid_utf8(ensure_utf8(ascii)));

        // Already-UTF-8 (English locale / working VSLANG) passes through unchanged.
        std::string utf8 = "error C2059: \xE8\xAA\x9E\xE6\xB3\x95 error";  // "語法" in UTF-8
        CHECK(is_valid_utf8(utf8));
        CHECK(ensure_utf8(utf8) == utf8);

        // Raw CP950 bytes (語法錯誤 = BB 79 AA 6B BF 79 BB 7E) are NOT valid UTF-8;
        // ensure_utf8 must transcode so the output is valid UTF-8 either way.
        std::string cp950 = "error C2059: \xBB\x79\xAA\x6B\xBF\x79\xBB\x7E: 'return'";
        CHECK(!is_valid_utf8(cp950));
        std::string fixed = ensure_utf8(cp950);
        CHECK(is_valid_utf8(fixed));   // the core invariant — no mojibake escapes
        // The ASCII skeleton (error code, quotes) survives intact.
        CHECK(fixed.find("error C2059:") != std::string::npos);
        CHECK(fixed.find("'return'") != std::string::npos);

        // Empty stays empty.
        CHECK(ensure_utf8("").empty());
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_diagnostics: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_diagnostics: %d FAIL\n", g_failures);
    return 1;
}
