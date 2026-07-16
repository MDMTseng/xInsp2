//
// test_image_pool_recycle.cpp — the pixpool size-class buffer recycler
// (perf/imagepool-sizeclass backport of the design-C magazine).
//
// Asserts the NEW allocation layer only; every handle/refcount/owner/sweep
// semantic is covered by test_image_pool / test_image_pool_stress unchanged.
//
//   1. Same-size create/release cycles are served by the per-thread magazine
//      (magazine_hits counter advances; no fresh heap alloc after warmup).
//   2. Every pool buffer is 64-byte aligned (SIMD/cacheline contract).
//   3. No zero-fill (CT ruling 2026-07): create() returns UNINITIALISED pixels;
//      a recycled buffer carries its previous life's stale bytes verbatim.
//   4. Budget eviction: pushing more frees than magazine+shelf budgets allow
//      routes the excess to _aligned_free (evicted_frees advances) and the
//      shelf never exceeds its per-class cap.
//   5. Cross-thread migration: created on thread A, released on thread B —
//      the buffer lands in B's magazine and B's next same-size create hits it
//      (correctness of migration, not affinity).
//   6. Above-max direct lane: > 64 MiB images bypass the magazines entirely
//      (direct_allocs/direct_frees advance; no magazine/shelf traffic).
//

#include <xi/xi_image_pool.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#define CHECK(expr)                                                  \
    do {                                                             \
        if (!(expr)) {                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                 \
                __FILE__, __LINE__, #expr);                          \
            std::abort();                                            \
        }                                                            \
    } while (0)

#define SECTION(name) std::fprintf(stderr, "\n[section] %s\n", name)

using Stats = xi::ImagePool::PixAllocStats;

// ---------- 1: same-size churn hits the magazine ---------------------

static void test_magazine_hit_on_same_size_churn() {
    SECTION("Same-size create/release cycles hit the per-thread magazine");
    auto& pool = xi::ImagePool::instance();

    // Warm the magazine: first cycle faults a fresh buffer in, release
    // caches it.
    xi_image_handle w = pool.create(1920, 1200, 1);
    CHECK(w != 0);
    pool.release(w);

    Stats before = xi::ImagePool::pixel_alloc_stats();
    constexpr int CYCLES = 100;
    for (int i = 0; i < CYCLES; ++i) {
        xi_image_handle h = pool.create(1920, 1200, 1);
        CHECK(h != 0);
        CHECK(pool.data(h) != nullptr);
        pool.release(h);
    }
    Stats after = xi::ImagePool::pixel_alloc_stats();

    // Every cycle reuses the SAME cached buffer: all magazine hits, zero
    // fresh heap allocs.
    CHECK(after.magazine_hits - before.magazine_hits == CYCLES);
    CHECK(after.fresh_allocs == before.fresh_allocs);
    CHECK(after.magazine_puts - before.magazine_puts == CYCLES);
    CHECK(pool.stats().handle_count == 0);
}

// ---------- 2: 64-byte alignment --------------------------------------

static void test_alignment() {
    SECTION("Every pool buffer is 64-byte aligned");
    auto& pool = xi::ImagePool::instance();
    // A spread of class sizes + the direct lane.
    const int dims[][3] = {
        {16, 16, 1},        // 256 B  -> min class
        {320, 240, 3},      // ~230 KB
        {1920, 1200, 1},    // ~2.3 MB
        {9000, 8000, 1},    // ~72 MB -> direct lane
    };
    for (auto& d : dims) {
        xi_image_handle h = pool.create(d[0], d[1], d[2]);
        CHECK(h != 0);
        uint8_t* p = pool.data(h);
        CHECK(p != nullptr);
        CHECK((reinterpret_cast<uintptr_t>(p) & 63u) == 0);
        pool.release(h);
    }
    CHECK(pool.stats().handle_count == 0);
}

// ---------- 3: NO zero-fill — a recycled buffer keeps its stale bytes --------

static void test_no_zero_fill_on_recycled_buffer() {
    SECTION("create() returns UNINITIALISED pixels — a recycled buffer carries "
            "stale bytes (CT ruling 2026-07: canvas zero-fill removed)");
    auto& pool = xi::ImagePool::instance();
    constexpr int W = 640, H = 480, C = 3;
    constexpr size_t N = size_t(W) * H * C;

    // Dirty a buffer, release it into the magazine.
    xi_image_handle a = pool.create(W, H, C);
    CHECK(a != 0);
    std::memset(pool.data(a), 0xDD, N);
    pool.release(a);

    // The next same-size create recycles that exact buffer (LIFO magazine). With
    // zero-fill removed, create() spends no memset — the 0xDD bytes survive
    // verbatim. (The producer is responsible for overwriting what it exposes.)
    Stats s0 = xi::ImagePool::pixel_alloc_stats();
    xi_image_handle b = pool.create(W, H, C);
    CHECK(b != 0);
    Stats s1 = xi::ImagePool::pixel_alloc_stats();
    CHECK(s1.magazine_hits == s0.magazine_hits + 1);   // proven recycled
    const uint8_t* p = pool.data(b);
    bool stale_preserved = true;
    for (size_t i = 0; i < N; ++i)
        if (p[i] != 0xDD) { stale_preserved = false; break; }
    CHECK(stale_preserved);
    pool.release(b);
    CHECK(pool.stats().handle_count == 0);
}

// ---------- 4: budget eviction ----------------------------------------

static void test_budget_eviction() {
    SECTION("Frees past magazine+shelf budgets go to the heap (evicted)");
    auto& pool = xi::ImagePool::instance();
    // 5 MB image -> 8 MiB class. Budgets: magazine 4/thread (32 MiB < the
    // 64 MiB byte budget), shelf 128 MiB / 8 MiB = 16. Holding 40 live then
    // releasing all 40 on one thread can cache at most 4 + 16 = 20; the
    // other 20 must be evicted to the heap.
    constexpr int W = 2500, H = 2000, C = 1;                  // 5 MB
    constexpr int COUNT = 40, CACHEABLE = 4 + 16;

    std::vector<xi_image_handle> hs;
    for (int i = 0; i < COUNT; ++i) {
        xi_image_handle h = pool.create(W, H, C);
        CHECK(h != 0);
        hs.push_back(h);
    }
    Stats before = xi::ImagePool::pixel_alloc_stats();
    for (auto h : hs) pool.release(h);
    Stats after = xi::ImagePool::pixel_alloc_stats();

    uint64_t cached = (after.magazine_puts - before.magazine_puts) +
                      (after.shelf_puts - before.shelf_puts);
    uint64_t evicted = after.evicted_frees - before.evicted_frees;
    // The shelf may have residue from earlier tests, so cached can come in
    // UNDER the cap (never over); everything not cached must be evicted.
    CHECK(cached <= CACHEABLE);
    CHECK(evicted >= COUNT - CACHEABLE);
    CHECK(cached + evicted == COUNT);
    CHECK(pool.stats().handle_count == 0);
}

// ---------- 5: cross-thread create-here-release-there ------------------

static void test_cross_thread_migration() {
    SECTION("Buffer created on A, released on B, recycled by B's next create");
    auto& pool = xi::ImagePool::instance();
    constexpr int W = 800, H = 600, C = 3;                    // ~1.4 MB class

    std::atomic<xi_image_handle> handoff{0};
    std::atomic<bool> a_done{false};

    std::thread producer([&] {
        xi_image_handle h = pool.create(W, H, C);
        CHECK(h != 0);
        pool.data(h)[0] = 0x5A;
        handoff.store(h, std::memory_order_release);
        a_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        while (!a_done.load(std::memory_order_acquire)) std::this_thread::yield();
        xi_image_handle h = handoff.load(std::memory_order_acquire);
        CHECK(pool.data(h) != nullptr);

        // This thread has released nothing yet, so ITS magazine is empty for
        // every class — the release below must land at its LIFO top and the
        // next same-size create must pop exactly that buffer.
        uint8_t* victim_mem = pool.data(h);
        Stats before = xi::ImagePool::pixel_alloc_stats();
        pool.release(h);                                       // A's buffer, B's magazine (LIFO top)
        Stats mid = xi::ImagePool::pixel_alloc_stats();
        CHECK(mid.magazine_puts == before.magazine_puts + 1);  // cached, not evicted

        xi_image_handle h2 = pool.create(W, H, C);             // must pop the LIFO top
        CHECK(h2 != 0);
        Stats afterS = xi::ImagePool::pixel_alloc_stats();
        CHECK(afterS.magazine_hits == mid.magazine_hits + 1);
        // LIFO: the recycled buffer IS the one A allocated (migrated to B).
        CHECK(pool.data(h2) == victim_mem);
        pool.release(h2);
    });

    producer.join();
    consumer.join();
    CHECK(pool.stats().handle_count == 0);
}

// ---------- 6: above-max direct lane ------------------------------------

static void test_direct_lane_above_max() {
    SECTION("> 64 MiB images take the direct heap lane, never cached");
    auto& pool = xi::ImagePool::instance();
    // 9000 x 8000 x 1 = ~68.7 MiB > the 64 MiB max class (pool cap is 1 GiB).
    constexpr int W = 9000, H = 8000, C = 1;

    Stats before = xi::ImagePool::pixel_alloc_stats();
    xi_image_handle h = pool.create(W, H, C);
    CHECK(h != 0);
    uint8_t* p = pool.data(h);
    CHECK(p != nullptr);
    CHECK((reinterpret_cast<uintptr_t>(p) & 63u) == 0);
    // Usable end-to-end.
    p[0] = 0x42;
    p[size_t(W) * H * C - 1] = 0x99;
    CHECK(pool.data(h)[size_t(W) * H * C - 1] == 0x99);
    pool.release(h);
    Stats after = xi::ImagePool::pixel_alloc_stats();

    CHECK(after.direct_allocs == before.direct_allocs + 1);
    CHECK(after.direct_frees  == before.direct_frees + 1);
    // No magazine/shelf traffic for the direct lane.
    CHECK(after.magazine_hits == before.magazine_hits);
    CHECK(after.magazine_puts == before.magazine_puts);
    CHECK(after.shelf_puts    == before.shelf_puts);
    CHECK(pool.stats().handle_count == 0);
}

int main() {
    std::fprintf(stderr, "=== test_image_pool_recycle ===\n");

    test_magazine_hit_on_same_size_churn();
    test_alignment();
    test_no_zero_fill_on_recycled_buffer();
    test_budget_eviction();
    test_cross_thread_migration();
    test_direct_lane_above_max();

    auto s = xi::ImagePool::pixel_alloc_stats();
    std::fprintf(stderr,
        "\npixpool: mag_hits=%llu shelf_hits=%llu fresh=%llu direct=%llu "
        "mag_puts=%llu shelf_puts=%llu evicted=%llu direct_frees=%llu\n",
        (unsigned long long)s.magazine_hits, (unsigned long long)s.shelf_hits,
        (unsigned long long)s.fresh_allocs, (unsigned long long)s.direct_allocs,
        (unsigned long long)s.magazine_puts, (unsigned long long)s.shelf_puts,
        (unsigned long long)s.evicted_frees, (unsigned long long)s.direct_frees);
    std::fprintf(stderr, "\nALL TESTS PASSED\n");
    return 0;
}
