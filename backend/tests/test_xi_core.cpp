//
// test_xi_core.cpp — M0 regression test (assertion-based).
//
// Covers every primitive in xi/xi.hpp with real assertions. Fails the build
// on regression. Header-only, no dependencies beyond STL.
//

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <xi/xi.hpp>

// Minimal test harness — each TEST() runs once; failures print and set a flag.
static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define SECTION(name) std::printf("[test] %s\n", name)

// ---------- xi_async ----------

static int slow_add(int a, int b, int delay_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    return a + b;
}

static int always_throws(int) {
    throw std::runtime_error("boom");
}

static void test_async_basic() {
    SECTION("async basic");
    auto f = xi::async(slow_add, 2, 3, 5);
    int v  = f;  // implicit await
    CHECK(v == 5);
}

static void test_async_parallel() {
    SECTION("async parallel wall time");
    using namespace std::chrono;
    auto t0 = steady_clock::now();
    auto a  = xi::async(slow_add, 1, 1, 50);
    auto b  = xi::async(slow_add, 2, 2, 50);
    auto c  = xi::async(slow_add, 3, 3, 50);
    int  ra = a, rb = b, rc = c;
    auto dt = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    CHECK(ra == 2);
    CHECK(rb == 4);
    CHECK(rc == 6);
    // Three 50ms tasks in parallel should finish in < 150ms (sequential)
    // and typically around 55-80ms. Allow generous slack for CI.
    std::printf("  parallel wall time: %lldms\n", (long long)dt);
    CHECK(dt < 130);
}

static void test_async_exception() {
    SECTION("async exception propagation");
    auto f = xi::async(always_throws, 42);
    bool caught = false;
    try {
        int v = f;  // implicit await should re-throw
        (void)v;
    } catch (const std::runtime_error& e) {
        caught = (std::string(e.what()) == "boom");
    }
    CHECK(caught);
}

static int square(int x) { return x * x; }
ASYNC_WRAP(square)

static void test_async_wrap() {
    SECTION("ASYNC_WRAP");
    auto f    = async_square(9);
    int  v    = f;
    CHECK(v == 81);
}

static std::atomic<int> side_effect{0};
static void bump() { side_effect.fetch_add(1); }

static void test_await_all_mixed_void() {
    SECTION("await_all accepts Future<void> + non-void, void is awaited but filtered out");
    side_effect.store(0);
    auto fv1 = xi::async(bump);
    auto fi  = xi::async([](){ return 7; });
    auto fv2 = xi::async(bump);
    auto fd  = xi::async([](){ return 3.5; });

    auto t = xi::await_all(fv1, fi, fv2, fd);
    static_assert(std::tuple_size_v<decltype(t)> == 2,
                  "void futures must contribute no tuple entry");
    CHECK(std::get<0>(t) == 7);
    CHECK(std::get<1>(t) > 3.4 && std::get<1>(t) < 3.6);
    CHECK(side_effect.load() == 2);
}

// ---------- xi_var ----------

static void test_var_basic() {
    SECTION("VAR tracks and binds");
    xi::ValueStore::current().clear();
    VAR(x, 42);
    VAR(y, 3.14);
    VAR(flag, true);
    VAR(name, std::string("hello"));
    CHECK(x == 42);
    CHECK(y > 3.13 && y < 3.15);
    CHECK(flag == true);
    CHECK(name == "hello");

    auto snap = xi::ValueStore::current().snapshot();
    CHECK(snap.size() == 4);
    CHECK(snap[0].name == "x");
    CHECK(snap[0].kind == xi::VarKind::Number);
    CHECK(snap[0].inline_json == "42");
    CHECK(snap[1].name == "y");
    CHECK(snap[1].kind == xi::VarKind::Number);
    CHECK(snap[2].name == "flag");
    CHECK(snap[2].kind == xi::VarKind::Boolean);
    CHECK(snap[2].inline_json == "true");
    CHECK(snap[3].name == "name");
    CHECK(snap[3].kind == xi::VarKind::String);
}

static void test_var_string_literal() {
    SECTION("VAR with string literal is copied into std::string, not stashed as const char*");
    xi::ValueStore::current().clear();
    VAR(greeting, "hello world");
    auto snap = xi::ValueStore::current().snapshot();
    CHECK(snap.size() == 1);
    CHECK(snap[0].kind == xi::VarKind::String);
    // Payload must own the string — an `any<const char*>` would deserialize wrong.
    CHECK(snap[0].payload.type() == typeid(std::string));
    CHECK(std::any_cast<std::string>(snap[0].payload) == "hello world");
}

static void test_var_thread_local() {
    SECTION("ValueStore is thread-local");
    xi::ValueStore::current().clear();
    VAR(a_main, 1);

    std::atomic<size_t> other_size{0};
    std::thread t([&] {
        xi::ValueStore::current().clear();
        VAR(b_child, 2);
        VAR(c_child, 3);
        other_size = xi::ValueStore::current().snapshot().size();
    });
    t.join();

    CHECK(other_size.load() == 2);
    CHECK(xi::ValueStore::current().snapshot().size() == 1);
}

// ---------- xi_param ----------

static void test_param_basic() {
    SECTION("Param implicit read + clamp");
    xi::Param<double> sigma{"test_sigma", 3.0, {0.1, 10.0}};
    double v = sigma;
    CHECK(v == 3.0);

    sigma.set(20.0);  // out of range, clamps to 10
    CHECK(static_cast<double>(sigma) == 10.0);
    sigma.set(-5.0);
    CHECK(static_cast<double>(sigma) == 0.1);

    // Registry lookup
    auto* found = xi::ParamRegistry::instance().find("test_sigma");
    CHECK(found != nullptr);
    CHECK(found->name() == "test_sigma");
    CHECK(found->type_name() == "float");

    // JSON round-trip via set_from_json
    CHECK(found->set_from_json("7.5"));
    CHECK(static_cast<double>(sigma) == 7.5);
    CHECK(!found->set_from_json("not_a_number"));
}

static void test_param_bool() {
    SECTION("Param<bool>");
    xi::Param<bool> flag{"test_flag", false};
    CHECK(static_cast<bool>(flag) == false);
    auto* p = xi::ParamRegistry::instance().find("test_flag");
    CHECK(p != nullptr);
    CHECK(p->set_from_json("true"));
    CHECK(static_cast<bool>(flag) == true);
    CHECK(!p->set_from_json("maybe"));
}

// ---------- xi_instance ----------

class DummyPlugin : public xi::InstanceBase {
public:
    explicit DummyPlugin(std::string n) : name_(std::move(n)) {}
    const std::string& name() const override { return name_; }
    std::string plugin_name() const override { return "DummyPlugin"; }
    int counter = 0;
private:
    std::string name_;
};

namespace xi {
template <>
std::shared_ptr<DummyPlugin> make_plugin_instance<DummyPlugin>(std::string_view name) {
    return std::make_shared<DummyPlugin>(std::string(name));
}
}

static void test_instance_basic() {
    SECTION("Instance<T> create + registry");
    xi::InstanceRegistry::instance().clear();
    xi::Instance<DummyPlugin> a{"plugin_a"};
    CHECK(a);
    CHECK(a->name() == "plugin_a");
    a->counter = 42;

    // Same name → reuses existing
    xi::Instance<DummyPlugin> a2{"plugin_a"};
    CHECK(a2);
    CHECK(a2->counter == 42);

    auto list = xi::InstanceRegistry::instance().list();
    CHECK(list.size() == 1);
    CHECK(list[0]->plugin_name() == "DummyPlugin");
}

// ---------- main ----------

int main() {
    test_async_basic();
    test_async_parallel();
    test_async_exception();
    test_async_wrap();

    test_await_all_mixed_void();
    test_var_basic();
    test_var_string_literal();
    test_var_thread_local();

    test_param_basic();
    test_param_bool();

    test_instance_basic();

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
        return 1;
    }
}
