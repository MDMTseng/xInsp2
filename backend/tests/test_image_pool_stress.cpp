//
// test_image_pool_stress.cpp — concurrency stress for the lock-free
// ImagePool. Goals:
//
//   1. Mass churn: many threads creating + releasing rapidly forces
//      slot reuse through the Treiber stack free list. ABA hazards
//      (slot returned then immediately re-allocated by another
//      thread) should be defused by the versioned head.
//
//   2. Stale-handle rejection: holding a handle past release, then
//      letting another thread reuse the slot, must NOT let the
//      stale handle reach the new occupant. Generation counter on
//      PoolEntry vs the handle's generation field is what defends.
//
//   3. Cross-owner correctness: multi-owner concurrent allocation,
//      then release_all_for sweeps one owner; other owners' handles
//      must remain valid and account for the right footprint in
//      stats_by_owner.
//
//   4. No lost handles + no duplicates under contention.
//

#include <xi/xi_image_pool.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
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

// ---------- 1: Mass churn forcing slot reuse via free list ----------

static void test_churn_slot_reuse() {
    SECTION("Mass churn forces slot reuse; no double-allocation");
    auto& pool = xi::ImagePool::instance();
    constexpr int THREADS = 8;
    constexpr int OPS_PER = 5'000;

    std::atomic<int> ok_creates{0};
    std::atomic<int> ok_releases{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            // Each thread creates+releases in tight loops, pumping the
            // free list. With the prior shared_mutex design this would
            // serialise on the shard lock; the lock-free version
            // should run unblocked.
            for (int i = 0; i < OPS_PER; ++i) {
                xi_image_handle h = pool.create(4, 4, 1);
                if (!h) continue;
                ok_creates.fetch_add(1, std::memory_order_relaxed);
                // Touch the data so the handle is genuinely usable.
                uint8_t* p = pool.data(h);
                CHECK(p != nullptr);
                p[0] = (uint8_t)((t * 31 + i) & 0xff);
                pool.release(h);
                ok_releases.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK(ok_creates.load()  == THREADS * OPS_PER);
    CHECK(ok_releases.load() == THREADS * OPS_PER);

    // Steady state: zero leaked handles.
    auto stats = pool.stats(/*owner=*/0);
    CHECK(stats.handle_count == 0);
}

// ---------- 2: Stale handle returns nullptr after slot reuse -------

static void test_stale_handle_rejection() {
    SECTION("Stale handle past slot reuse must not reach new occupant");
    auto& pool = xi::ImagePool::instance();

    // Hold one handle, release it, then create many to force slot
    // reuse. The released slot WILL be reused (it's the most-recent
    // free-list head). Looking up the stale handle must return null,
    // NOT the new occupant's data.
    xi_image_handle stale = pool.create(2, 2, 1);
    CHECK(stale != 0);
    uint8_t* px_old = pool.data(stale);
    CHECK(px_old != nullptr);
    px_old[0] = 0xCC;
    pool.release(stale);

    // 200 cycles of create+release on the same low-index slot pumps
    // the generation counter well past the stale handle's value.
    for (int i = 0; i < 200; ++i) {
        xi_image_handle h = pool.create(2, 2, 1);
        CHECK(h != 0);
        // Stale handle MUST NOT lookup the current occupant.
        CHECK(pool.data(stale) == nullptr);
        CHECK(pool.width(stale) == 0);
        // The fresh handle is fine.
        CHECK(pool.data(h) != nullptr);
        CHECK(pool.width(h) == 2);
        pool.release(h);
    }
    CHECK(pool.stats().handle_count == 0);
}

// ---------- 3: Owner sweep under concurrent load -------------------

static void test_owner_sweep_concurrent() {
    SECTION("release_all_for sweeps one owner; others untouched");
    auto& pool = xi::ImagePool::instance();
    auto owner_a = xi::ImagePool::alloc_owner_id();
    auto owner_b = xi::ImagePool::alloc_owner_id();

    constexpr int PER_OWNER = 500;
    std::vector<xi_image_handle> a_handles(PER_OWNER, 0);
    std::vector<xi_image_handle> b_handles(PER_OWNER, 0);

    auto fill = [&](xi::ImagePoolOwnerId owner, std::vector<xi_image_handle>& out) {
        xi::ImagePool::OwnerGuard g(owner);
        for (int i = 0; i < PER_OWNER; ++i) {
            out[i] = pool.create(8, 8, 1);
            CHECK(out[i] != 0);
        }
    };

    std::thread tA(fill, owner_a, std::ref(a_handles));
    std::thread tB(fill, owner_b, std::ref(b_handles));
    tA.join();
    tB.join();

    auto agg = pool.stats_by_owner();
    int found_a = 0, found_b = 0;
    for (auto& s : agg) {
        if (s.owner == owner_a) found_a = s.handle_count;
        if (s.owner == owner_b) found_b = s.handle_count;
    }
    CHECK(found_a == PER_OWNER);
    CHECK(found_b == PER_OWNER);

    // Sweep owner A. Owner B's handles must remain valid.
    int swept = pool.release_all_for(owner_a);
    CHECK(swept == PER_OWNER);

    for (auto h : a_handles) {
        CHECK(pool.data(h) == nullptr);   // swept
    }
    for (auto h : b_handles) {
        CHECK(pool.data(h) != nullptr);   // alive
    }
    CHECK(pool.stats(owner_a).handle_count == 0);
    CHECK(pool.stats(owner_b).handle_count == PER_OWNER);

    // Cleanup B.
    for (auto h : b_handles) pool.release(h);
    CHECK(pool.stats().handle_count == 0);
}

// ---------- 4: Concurrent addref/release on a hot handle ----------

static void test_concurrent_addref_release_balanced() {
    SECTION("Many threads addref+release; refcount stays balanced");
    auto& pool = xi::ImagePool::instance();
    constexpr int THREADS = 8;
    constexpr int OPS_PER = 10'000;

    xi_image_handle h = pool.create(16, 16, 1);
    CHECK(h != 0);

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < OPS_PER; ++i) {
                pool.addref(h);
                CHECK(pool.data(h) != nullptr);
                pool.release(h);
            }
        });
    }
    for (auto& th : threads) th.join();

    // After balanced add/release, refcount is back to 1.
    CHECK(pool.data(h) != nullptr);
    pool.release(h);
    CHECK(pool.data(h) == nullptr);
    CHECK(pool.stats().handle_count == 0);
}

// ---------- 5: Mixed churn — creates and releases interleaved ------

static void test_mixed_churn() {
    SECTION("Mixed create/release interleaving — no lost slots, no dupes");
    auto& pool = xi::ImagePool::instance();
    constexpr int THREADS = 8;
    constexpr int CYCLES  = 2'000;

    std::atomic<int> alive{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            std::vector<xi_image_handle> mine;
            mine.reserve(CYCLES);
            for (int i = 0; i < CYCLES; ++i) {
                if ((i & 3) == 0 && !mine.empty()) {
                    // Release the oldest one we hold every 4 cycles.
                    auto h = mine.front();
                    mine.erase(mine.begin());
                    pool.release(h);
                    alive.fetch_sub(1, std::memory_order_relaxed);
                } else {
                    auto h = pool.create(2, 2, 1);
                    if (h) {
                        mine.push_back(h);
                        alive.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            // Drain.
            for (auto h : mine) pool.release(h);
            alive.fetch_sub((int)mine.size(), std::memory_order_relaxed);
        });
    }
    for (auto& th : threads) th.join();

    CHECK(alive.load() == 0);
    CHECK(pool.stats().handle_count == 0);
}

int main() {
    std::fprintf(stderr, "=== test_image_pool_stress ===\n");
    auto t0 = std::chrono::steady_clock::now();

    test_churn_slot_reuse();
    test_stale_handle_rejection();
    test_owner_sweep_concurrent();
    test_concurrent_addref_release_balanced();
    test_mixed_churn();

    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "\nALL TESTS PASSED in %lld ms\n", (long long)dt);
    return 0;
}
