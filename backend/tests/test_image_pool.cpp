//
// test_image_pool.cpp — ImagePool unit tests.
//
// Covers creation, refcounting, concurrency, round-trip, shard distribution,
// and large image handling. Follows the CHECK/SECTION pattern from
// test_xi_core.cpp.
//

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include <xi/xi_image_pool.hpp>

// Minimal test harness (same as test_xi_core.cpp).
static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define SECTION(name) std::printf("[test] %s\n", name)

// ---------- Test 1: Create + data + release ----------

static void test_create_data_release() {
    SECTION("Create + data + release");
    auto& pool = xi::ImagePool::instance();
    xi_image_handle h = pool.create(64, 48, 3);
    CHECK(h != 0);

    uint8_t* ptr = pool.data(h);
    CHECK(ptr != nullptr);

    // Pixels should be writable without crash.
    ptr[0] = 0xAB;
    ptr[1] = 0xCD;
    CHECK(ptr[0] == 0xAB);
    CHECK(ptr[1] == 0xCD);

    pool.release(h);
}

// ---------- Test 2: Addref + double release ----------

static void test_addref_double_release() {
    SECTION("Addref + double release (refcount 1->2->1->0)");
    auto& pool = xi::ImagePool::instance();
    xi_image_handle h = pool.create(4, 4, 1);

    // refcount starts at 1; addref bumps to 2
    pool.addref(h);

    // First release: refcount 2->1, data still alive
    pool.release(h);
    CHECK(pool.data(h) != nullptr);

    // Second release: refcount 1->0, entry freed
    pool.release(h);
    CHECK(pool.data(h) == nullptr);
}

// ---------- Test 3: Release twice (double-free guard) ----------

static void test_double_free_guard() {
    SECTION("Release twice (double-free guard)");
    auto& pool = xi::ImagePool::instance();
    xi_image_handle h = pool.create(4, 4, 1);

    pool.release(h);  // refcount 1->0, freed
    pool.release(h);  // should be a no-op, no crash
    // If we reach here without crashing, the guard works.
    CHECK(pool.data(h) == nullptr);
}

// ---------- Test 4: Data after release returns null ----------

static void test_data_after_release() {
    SECTION("Data after release returns null");
    auto& pool = xi::ImagePool::instance();
    xi_image_handle h = pool.create(8, 8, 3);
    CHECK(pool.data(h) != nullptr);

    pool.release(h);
    CHECK(pool.data(h) == nullptr);
}

// ---------- Test 5: Concurrent create from 4 threads ----------

static void test_concurrent_create() {
    SECTION("Concurrent create from 4 threads");
    auto& pool = xi::ImagePool::instance();
    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 100;

    std::vector<std::vector<xi_image_handle>> results(THREADS);

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            results[t].reserve(PER_THREAD);
            for (int i = 0; i < PER_THREAD; ++i) {
                results[t].push_back(pool.create(2, 2, 1));
            }
        });
    }
    for (auto& th : threads) th.join();

    // All handles must be unique and non-zero.
    std::set<xi_image_handle> all;
    for (int t = 0; t < THREADS; ++t) {
        for (auto h : results[t]) {
            CHECK(h != 0);
            all.insert(h);
        }
    }
    CHECK(all.size() == THREADS * PER_THREAD);

    // Cleanup
    for (int t = 0; t < THREADS; ++t)
        for (auto h : results[t])
            pool.release(h);
}

// ---------- Test 6: Concurrent addref/release from 4 threads ----------

static void test_concurrent_addref_release() {
    SECTION("Concurrent addref/release from 4 threads");
    auto& pool = xi::ImagePool::instance();
    constexpr int THREADS = 4;
    constexpr int OPS = 200;

    xi_image_handle h = pool.create(4, 4, 1);

    // Bump refcount up by THREADS * OPS so we can release that many times.
    for (int i = 0; i < THREADS * OPS; ++i)
        pool.addref(h);

    // Each thread releases OPS times concurrently.
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < OPS; ++i) {
                pool.release(h);
            }
        });
    }
    for (auto& th : threads) th.join();

    // After all concurrent releases, refcount should be back to 1 (the initial).
    // Data should still be accessible.
    CHECK(pool.data(h) != nullptr);

    // Final release brings it to 0.
    pool.release(h);
    CHECK(pool.data(h) == nullptr);
}

// ---------- Test 7: Width/height/channels correct ----------

static void test_dimensions() {
    SECTION("Width/height/channels correct");
    auto& pool = xi::ImagePool::instance();
    xi_image_handle h = pool.create(320, 240, 3);

    CHECK(pool.width(h) == 320);
    CHECK(pool.height(h) == 240);
    CHECK(pool.channels(h) == 3);

    pool.release(h);
}

// ---------- Test 8: from_image + to_image round-trip ----------

static void test_round_trip() {
    SECTION("from_image + to_image round-trip");
    auto& pool = xi::ImagePool::instance();

    // Build a small test image with known pixel data.
    xi::Image src(4, 4, 3);
    for (size_t i = 0; i < src.size(); ++i)
        src.data()[i] = static_cast<uint8_t>(i & 0xFF);

    xi_image_handle h = pool.from_image(src);
    CHECK(h != 0);

    xi::Image dst = pool.to_image(h);
    CHECK(dst.width == src.width);
    CHECK(dst.height == src.height);
    CHECK(dst.channels == src.channels);
    CHECK(dst.size() == src.size());
    CHECK(std::memcmp(dst.data(), src.data(), src.size()) == 0);

    pool.release(h);
}

// ---------- Test 9: Shard distribution ----------

static void test_shard_distribution() {
    SECTION("Shard distribution — 16 consecutive handles hit different shards");
    auto& pool = xi::ImagePool::instance();

    std::vector<xi_image_handle> handles;
    for (int i = 0; i < 16; ++i)
        handles.push_back(pool.create(1, 1, 1));

    // Count how many distinct shards (lower 4 bits) are hit.
    std::set<int> shards;
    for (auto h : handles)
        shards.insert(static_cast<int>(h & 0xF));

    // With sequential handle allocation, consecutive handles should hit
    // all 16 shards (or at least many of them). Require > 1 to prove
    // distribution is not degenerate.
    std::printf("  distinct shards hit: %d / 16\n", (int)shards.size());
    CHECK(shards.size() > 1);

    for (auto h : handles) pool.release(h);
}

// ---------- Test 10: Large image (20MP) ----------

static void test_large_image() {
    SECTION("Large image (20MP)");
    auto& pool = xi::ImagePool::instance();

    // 5472 x 3648 x 3 = ~59.8 MB (roughly a 20-megapixel RGB image)
    constexpr int32_t W = 5472, H = 3648, C = 3;
    xi_image_handle h = pool.create(W, H, C);
    CHECK(h != 0);

    uint8_t* ptr = pool.data(h);
    CHECK(ptr != nullptr);

    // Write first and last pixels.
    ptr[0] = 0x42;
    size_t last = (size_t)W * H * C - 1;
    ptr[last] = 0x99;

    // Read them back.
    CHECK(pool.data(h)[0] == 0x42);
    CHECK(pool.data(h)[last] == 0x99);

    CHECK(pool.width(h) == W);
    CHECK(pool.height(h) == H);
    CHECK(pool.channels(h) == C);

    pool.release(h);
}

// ---------- main ----------

int main() {
    test_create_data_release();
    test_addref_double_release();
    test_double_free_guard();
    test_data_after_release();
    test_concurrent_create();
    test_concurrent_addref_release();
    test_dimensions();
    test_round_trip();
    test_shard_distribution();
    test_large_image();

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
        return 1;
    }
}
