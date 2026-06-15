// test_doc_pool.cpp — unit test for the γ pooled doc allocator (xi_doc_pool.hpp).
// Exercises alloc/realloc/free across size classes + boundaries, reuse, and a
// real yyjson_mut_doc round-trip driven entirely through the pool allocator.

#include "xi/xi_doc_pool.hpp"
#include "yyjson.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

using xi::DocChunkPool;

// Fill a block with a size-derived byte pattern, then verify it — catches
// header/payload overlap, wrong-size returns, and reuse stomping.
static void fill(void* p, size_t n, uint8_t seed) { std::memset(p, seed, n); }
static bool check_fill(void* p, size_t n, uint8_t seed) {
    auto* b = static_cast<uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) if (b[i] != seed) return false;
    return true;
}

static void test_basic_sizes() {
    // Across every class boundary and a couple oversize values.
    size_t sizes[] = { 1, 8, 16, 48, 49, 64, 112, 113, 1000, 4096,
                       65000, 65520, 65521, 200000 };
    for (size_t s : sizes) {
        void* p = DocChunkPool::alloc(s);
        CHECK(p != nullptr, "alloc returned null");
        CHECK(((uintptr_t)p % 16) == 0, "alloc not 16-aligned");
        fill(p, s, 0xAB);
        CHECK(check_fill(p, s, 0xAB), "payload corrupted after fill");
        DocChunkPool::free(p);
    }
}

static void test_reuse() {
    // Free then re-alloc in the same class should hand back a reused block
    // (same pointer) — proves the free-list link/header round-trips.
    // Both totals (16+100=116, 16+90=106) fall in the 128-byte class, so the
    // freed block must be handed back.
    void* a = DocChunkPool::alloc(100);
    DocChunkPool::free(a);
    void* b = DocChunkPool::alloc(90);
    CHECK(a == b, "expected free-list reuse of same-class block");
    DocChunkPool::free(b);
}

static void test_realloc() {
    void* p = DocChunkPool::alloc(40);
    fill(p, 40, 0x11);
    // grow within class (40 -> 48, both fit a 64-total block) → same ptr
    void* p2 = DocChunkPool::realloc(p, 44);
    CHECK(p2 == p, "in-class grow should keep the pointer");
    CHECK(check_fill(p2, 40, 0x11), "data lost on in-class realloc");
    // grow across classes → new block, data preserved
    void* p3 = DocChunkPool::realloc(p2, 5000);
    CHECK(check_fill(p3, 40, 0x11), "data lost on cross-class realloc grow");
    // realloc(nullptr) == alloc
    void* p4 = DocChunkPool::realloc(nullptr, 32);
    CHECK(p4 != nullptr, "realloc(nullptr) should alloc");
    DocChunkPool::free(p3);
    DocChunkPool::free(p4);
}

static void test_stress() {
    // Deterministic pseudo-random churn — no Math.random; an LCG.
    std::vector<std::pair<void*, std::pair<size_t, uint8_t>>> live;
    uint32_t rng = 0x12345678;
    auto next = [&] { rng = rng * 1664525u + 1013904223u; return rng; };
    for (int i = 0; i < 20000; ++i) {
        if ((next() & 1) || live.empty()) {
            size_t s = 1 + (next() % 9000);
            void* p = DocChunkPool::alloc(s);
            CHECK(p != nullptr, "stress alloc null");
            uint8_t seed = (uint8_t)next();
            fill(p, s, seed);
            live.push_back({ p, { s, seed } });
        } else {
            size_t idx = next() % live.size();
            auto& e = live[idx];
            CHECK(check_fill(e.first, e.second.first, e.second.second),
                  "stress: live block corrupted before free");
            DocChunkPool::free(e.first);
            live[idx] = live.back();
            live.pop_back();
        }
    }
    for (auto& e : live) {
        CHECK(check_fill(e.first, e.second.first, e.second.second),
              "stress: live block corrupted at teardown");
        DocChunkPool::free(e.first);
    }
}

static void* pool_malloc (void*, size_t s)            { return DocChunkPool::alloc(s); }
static void* pool_realloc(void*, void* p, size_t, size_t s) { return DocChunkPool::realloc(p, s); }
static void  pool_free   (void*, void* p)             { DocChunkPool::free(p); }

static void test_yyjson_roundtrip() {
    yyjson_alc alc{ pool_malloc, pool_realloc, pool_free, nullptr };

    // Build a non-trivial mutable doc entirely through the pool allocator.
    yyjson_mut_doc* doc = yyjson_mut_doc_new(&alc);
    CHECK(doc != nullptr, "mut_doc_new via pool failed");
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "count", 42);
    yyjson_mut_obj_add_strcpy(doc, root, "label", "hello pool");
    yyjson_mut_val* arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "items", arr);
    for (int i = 0; i < 500; ++i) {
        yyjson_mut_val* o = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, o, "i", i);
        yyjson_mut_obj_add_strcpy(doc, o, "name", "an item with a moderately long string value");
        yyjson_mut_arr_add_val(arr, o);
    }

    // Serialize (writer also uses the pool alc), read back, verify content.
    size_t len = 0;
    char* js = yyjson_mut_write_opts(doc, 0, &alc, &len, nullptr);
    CHECK(js != nullptr && len > 0, "mut_write via pool failed");

    yyjson_doc* rd = yyjson_read_opts(js, len, 0, &alc, nullptr);
    CHECK(rd != nullptr, "read_opts via pool failed");
    yyjson_val* rroot = yyjson_doc_get_root(rd);
    CHECK(yyjson_get_int(yyjson_obj_get(rroot, "count")) == 42, "count round-trip");
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(rroot, "label")), "hello pool") == 0, "label round-trip");
    CHECK(yyjson_arr_size(yyjson_obj_get(rroot, "items")) == 500, "array size round-trip");

    alc.free(alc.ctx, js);
    yyjson_doc_free(rd);
    yyjson_mut_doc_free(doc);
}

int main() {
    std::printf("test_doc_pool\n");
    test_basic_sizes();
    test_reuse();
    test_realloc();
    test_stress();
    test_yyjson_roundtrip();
    if (g_fail == 0) { std::printf("  OK (all checks passed)\n"); return 0; }
    std::printf("  %d check(s) FAILED\n", g_fail);
    return 1;
}
