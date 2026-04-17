#pragma once
//
// xi_test.hpp — a tiny test framework (~100 lines, zero dependencies).
//
// Usage:
//
//   XI_TEST(my_feature_works) {
//       XI_EXPECT(compute() == 42);
//       XI_EXPECT_EQ(greet("x"), "hello x");
//   }
//
//   int main() { return xi::test::run_all(); }
//
// Tests auto-register via static initializers. The runner prints one
// line per test and returns 0 if all passed, 1 otherwise.
//

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace xi::test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

struct Result {
    std::string name;
    bool passed;
    std::string error;
    double ms;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

struct Failure {
    std::string what;
};

inline void fail(const std::string& msg) { throw Failure{msg}; }

inline std::vector<Result> run_all(bool verbose = true) {
    std::vector<Result> results;
    int pass = 0, fail = 0;
    for (auto& tc : registry()) {
        auto t0 = std::chrono::steady_clock::now();
        Result r{tc.name, true, "", 0};
        try {
            tc.fn();
        } catch (const Failure& f) {
            r.passed = false;
            r.error = f.what;
        } catch (const std::exception& e) {
            r.passed = false;
            r.error = std::string("exception: ") + e.what();
        } catch (...) {
            r.passed = false;
            r.error = "unknown exception";
        }
        auto t1 = std::chrono::steady_clock::now();
        r.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (r.passed) ++pass; else ++fail;
        if (verbose) {
            std::fprintf(stderr, "  [%s] %s %s (%.1fms)%s%s\n",
                r.passed ? "PASS" : "FAIL",
                r.name.c_str(),
                r.passed ? "" : "—",
                r.ms,
                r.passed ? "" : "\n    ",
                r.passed ? "" : r.error.c_str());
        }
        results.push_back(std::move(r));
    }
    if (verbose) {
        std::fprintf(stderr, "\n%d passed, %d failed, %zu total\n",
                     pass, fail, registry().size());
    }
    return results;
}

} // namespace xi::test

// Register a test. The body runs as a lambda that throws on failure.
#define XI_TEST(name)                                                          \
    static void xi_test_##name();                                              \
    static ::xi::test::Registrar xi_test_reg_##name(#name, xi_test_##name);    \
    static void xi_test_##name()

// Assertion macros. They throw on failure with file:line + expression.
#define XI_EXPECT(expr)                                                        \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::ostringstream __oss;                                          \
            __oss << __FILE__ << ":" << __LINE__                               \
                  << " EXPECT failed: " #expr;                                 \
            ::xi::test::fail(__oss.str());                                     \
        }                                                                      \
    } while (0)

#define XI_EXPECT_EQ(a, b)                                                     \
    do {                                                                       \
        auto __a = (a); auto __b = (b);                                        \
        if (!(__a == __b)) {                                                   \
            std::ostringstream __oss;                                          \
            __oss << __FILE__ << ":" << __LINE__                               \
                  << " EXPECT_EQ failed: " #a " (" << __a                      \
                  << ") == " #b " (" << __b << ")";                            \
            ::xi::test::fail(__oss.str());                                     \
        }                                                                      \
    } while (0)
