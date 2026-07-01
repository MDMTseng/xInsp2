//
// test_doc_registry.cpp — unit tests for xi::DocRegistry (xi_doc_registry.hpp).
//
// The host-side refcount that backs host_api.doc_retain / doc_release (γ-4):
// a yyjson_mut_doc* shared across the plugin ABI is owned by the registry and
// freed when the last holder releases. Standalone, links yyjson only.
//

#include <cstdio>
#include <thread>
#include <vector>

#include <xi/xi_doc_registry.hpp>
#include "yyjson.h"

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

// Build a tiny owned doc. The registry doesn't care which allocator backs it,
// so the default yyjson allocator is fine for the test.
static yyjson_mut_doc* make_doc() {
    yyjson_mut_doc* d = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    return d;
}

// ---------- Test 1: retain / release lifecycle ----------

static void test_retain_release() {
    SECTION("retain/release lifecycle");
    auto& reg = xi::DocRegistry::instance();
    yyjson_mut_doc* d = make_doc();

    CHECK(reg.refcount(d) == 0);          // not registered yet
    reg.addref(d);
    CHECK(reg.refcount(d) == 1);          // first share registers at 1
    reg.addref(d);
    CHECK(reg.refcount(d) == 2);          // second holder
    reg.release(d);
    CHECK(reg.refcount(d) == 1);          // still one holder, NOT freed
    reg.release(d);                       // last holder → frees the doc
    CHECK(reg.refcount(d) == 0);          // gone from the registry
    // d is freed now — do not dereference it. refcount() only hashes the
    // pointer, never derefs, so this is safe.
}

// ---------- Test 2: release of an unregistered doc is a no-op ----------

static void test_release_unregistered() {
    SECTION("release unregistered = no-op");
    auto& reg = xi::DocRegistry::instance();
    yyjson_mut_doc* d = make_doc();
    CHECK(reg.refcount(d) == 0);
    reg.release(d);                       // defensive no-op; must NOT free or crash
    CHECK(reg.refcount(d) == 0);
    yyjson_mut_doc_free(d);               // we still own it — free manually
}

// ---------- Test 3: null is tolerated ----------

static void test_null_safe() {
    SECTION("null doc tolerated");
    auto& reg = xi::DocRegistry::instance();
    reg.addref(nullptr);
    reg.release(nullptr);
    CHECK(reg.refcount(nullptr) == 0);
}

// ---------- Test 4: many independent docs, no leak ----------

static void test_many_docs_no_leak() {
    SECTION("many docs, balanced retain/release leaves registry empty");
    auto& reg = xi::DocRegistry::instance();
    size_t base = reg.live_count();

    std::vector<yyjson_mut_doc*> docs;
    for (int i = 0; i < 200; ++i) {
        yyjson_mut_doc* d = make_doc();
        docs.push_back(d);
        reg.addref(d);
        reg.addref(d);                    // rc=2 each
    }
    CHECK(reg.live_count() == base + 200);

    for (auto* d : docs) {
        reg.release(d);                   // rc=1
        CHECK(reg.refcount(d) == 1);
    }
    CHECK(reg.live_count() == base + 200);

    for (auto* d : docs) reg.release(d);  // rc=0 → freed + erased
    CHECK(reg.live_count() == base);
}

// ---------- Test 5: concurrent retain/release on distinct docs ----------

static void test_concurrent() {
    SECTION("concurrent retain/release (distinct docs per thread)");
    auto& reg = xi::DocRegistry::instance();
    size_t base = reg.live_count();

    constexpr int THREADS = 4;
    constexpr int PER = 100;
    std::vector<std::thread> workers;
    for (int t = 0; t < THREADS; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < PER; ++i) {
                yyjson_mut_doc* d = make_doc();
                reg.addref(d);            // rc=1
                reg.addref(d);            // rc=2
                reg.release(d);           // rc=1
                reg.release(d);           // rc=0 → freed
            }
        });
    }
    for (auto& w : workers) w.join();
    // Everything balanced → registry back to baseline.
    CHECK(reg.live_count() == base);
}

int main() {
    test_retain_release();        // 1
    test_release_unregistered();  // 2
    test_null_safe();             // 3
    test_many_docs_no_leak();     // 4
    test_concurrent();            // 5

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
        return 1;
    }
}
