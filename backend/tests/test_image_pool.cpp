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

// ---------- Test: ImagePoolOwnerScope (RAII owner tagging) ----------

static void test_owner_scope() {
    SECTION("ImagePoolOwnerScope sweeps on drop, transfers on release");
    auto& pool = xi::ImagePool::instance();

    // (1) Images allocated inside run_factory are tagged to the scope's owner;
    //     if the scope is NOT released, its destructor sweeps them (no leak) —
    //     this is the failed-ctor path that used to leak when a site forgot the
    //     manual release_all_for.
    xi::ImagePoolOwnerId leaked_owner = 0;
    {
        xi::ImagePoolOwnerScope owner;
        leaked_owner = owner.id();
        owner.run_factory([&]() -> void* {
            pool.create(8, 8, 1);
            pool.create(8, 8, 1);
            return nullptr;   // simulate a failed/null ctor
        });
        CHECK(pool.stats(owner.id()).handle_count == 2);   // tagged + live in scope
    }   // not released -> dtor sweeps
    CHECK(pool.stats(leaked_owner).handle_count == 0);     // swept: no leak

    // (2) After release(), the scope must NOT sweep — ownership moved on (to the
    //     adapter in production); the images survive.
    xi::ImagePoolOwnerId kept_owner = 0;
    {
        xi::ImagePoolOwnerScope owner;
        kept_owner = owner.id();
        owner.run_factory([&]() -> void* { pool.create(8, 8, 1); return (void*)1; });
        CHECK(pool.stats(owner.id()).handle_count == 1);
        CHECK(owner.release() == kept_owner);
    }   // released -> dtor does NOT sweep
    CHECK(pool.stats(kept_owner).handle_count == 1);       // survived the scope

    pool.release_all_for(kept_owner);                      // cleanup (as the adapter dtor would)
    CHECK(pool.stats(kept_owner).handle_count == 0);
}

// ---------- Test: owner-sweep spares still-referenced (zero-copy) handles ----

// Minimal host_api over the singleton ImagePool, so we can drive the real
// xi::Image::adopt_pool_handle path a consumer instance Q uses to cache a frame
// zero-copy. Mirrors the wiring ImagePool::make_host_api() would give a plugin.
static const xi_host_api& test_host_api() {
    static xi_host_api api = xi::ImagePool::make_host_api();
    return api;
}

static void test_owner_sweep_spares_referenced() {
    SECTION("Owner-sweep spares handles a consumer still holds (zero-copy UAF guard)");
    auto& pool = xi::ImagePool::instance();
    const xi_host_api* host = &test_host_api();

    // (A) Producer P allocates a frame under its owner tag and writes a sentinel.
    xi::ImagePoolOwnerId P = xi::ImagePool::alloc_owner_id();
    xi_image_handle h = 0;
    {
        xi::ImagePool::OwnerGuard g(P);
        h = pool.create(8, 8, 1);   // refcount=1, owner=P
    }
    CHECK(h != 0);
    {
        uint8_t* px = pool.data(h);
        CHECK(px != nullptr);
        for (size_t i = 0; i < 64; ++i) px[i] = static_cast<uint8_t>(0xA0 + (i & 0x0F));
    }

    // (B) Consumer Q adopts the SAME handle (addref + caches the raw pixel
    //     pointer inside the xi::Image), simulating a buffer_replay / accumulator
    //     instance caching the frame across calls. refcount now 2 (P's ownership
    //     ref + Q's adopt ref), owner still P. We deliberately do NOT release P's
    //     create ref here: the owner-sweep is what drops P's ownership claim.
    xi::Image q_cached = xi::Image::adopt_pool_handle(host, h);
    CHECK(!q_cached.empty());
    CHECK(q_cached.width == 8 && q_cached.height == 8 && q_cached.channels == 1);

    // (C) P's adapter is destroyed (hot-recompile / remove / rename): the sweep
    //     drops P's one ownership ref. Q still holds its adopt ref, so the entry
    //     must survive (refcount 2 -> 1), not be force-freed.
    int swept = pool.release_all_for(P);
    // Q still holds a live ref → nothing reclaimed as a "leak".
    CHECK(swept == 0);

    // (D) Q's cached image MUST still be valid (no UAF, no freed memory): the
    //     sentinel bytes survive and remain readable through Q's cached data().
    CHECK(!q_cached.empty());
    const uint8_t* qpx = q_cached.data();
    CHECK(qpx != nullptr);
    bool sentinel_ok = true;
    for (size_t i = 0; i < 64; ++i)
        if (qpx[i] != static_cast<uint8_t>(0xA0 + (i & 0x0F))) { sentinel_ok = false; break; }
    CHECK(sentinel_ok);
    // The handle must still resolve through lookup() for Q (slot/gen intact).
    CHECK(pool.data(h) != nullptr);
    CHECK(pool.data(h) == qpx);

    // (E) When Q finally drops the frame, it is freed normally (no leak).
    q_cached = xi::Image{};   // releases Q's last ref
    CHECK(pool.data(h) == nullptr);

    // COMPLEMENT: an entry P owns with NO external consumer is still reclaimed by
    // release_all_for(P) — the leak-sweep intent is preserved.
    xi::ImagePoolOwnerId P2 = xi::ImagePool::alloc_owner_id();
    xi_image_handle leaked = 0;
    {
        xi::ImagePool::OwnerGuard g(P2);
        leaked = pool.create(8, 8, 1);   // refcount=1, owner=P2, nobody else
    }
    CHECK(leaked != 0);
    CHECK(pool.data(leaked) != nullptr);
    int swept2 = pool.release_all_for(P2);
    CHECK(swept2 == 1);                   // sole-owner leak reclaimed
    CHECK(pool.data(leaked) == nullptr);  // freed
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
    test_owner_scope();
    test_owner_sweep_spares_referenced();

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
        return 1;
    }
}
