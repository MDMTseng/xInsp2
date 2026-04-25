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

    if (g_failures == 0) {
        std::fprintf(stderr, "test_diagnostics: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_diagnostics: %d FAIL\n", g_failures);
    return 1;
}
