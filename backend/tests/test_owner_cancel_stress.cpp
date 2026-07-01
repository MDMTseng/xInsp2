//
// test_owner_cancel_stress.cpp — high-stress race nets for two §28 Tier-2/3 GAPs
// that had only functional (non-stress) coverage:
//
//   * C1/C2/C3 image-pool OWNER PROPAGATION across xi::async / xi::parallel_for.
//     test_parallel_safety proves it works with ONE image; this hammers it from
//     many host threads at once, each running its own owner epoch, so the pool's
//     per-owner accounting + the OwnerScope install/restore on omp/async workers
//     are exercised under real contention (not one image, one thread).
//
//   * WATCHDOG epoch-cancel (xi_async.hpp begin_inspect/arm_cancel/clear_cancel/
//     cancellation_requested) — previously a GAP. Two nets: a cross-thread
//     epoch-semantics assertion (in-flight inspects get cancelled, inspects that
//     start after the trip are spared) and a high-contention arm/clear-vs-
//     begin/poll loop that must never corrupt the monotonic ticket counter or
//     hang.
//
// The status coalesce map (§28 Tier-3) is intentionally NOT covered here: it's a
// `static` mutex-guarded std::map inside service_main.cpp (set_status_internal),
// not header-isolatable, and already the solved lock pattern — it stays covered
// black-box by the qa_* WS tests. Flagged in core_fix_plan.md §28 rather than
// gold-plated with an exe-linked harness.
//
// No TSan on MSVC (OQ-1); run under -DXINSP2_SANITIZE=address and the *_heavy
// XINSP2_STRESS_SCALE variant for the systematic-ish net.
//

#include <xi/xi_async.hpp>       // async, begin_inspect/arm_cancel/clear_cancel, owner thunks
#include <xi/xi_parallel.hpp>    // xi::parallel_for
#include <xi/xi_image_pool.hpp>  // ImagePool + make_host_api + OwnerGuard

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>
#include <vector>

static std::atomic<int> g_failures{0};
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures.fetch_add(1);                                          \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

static int stress_scale() {
    static int s = [] {
        const char* e = std::getenv("XINSP2_STRESS_SCALE");
        int v = e ? std::atoi(e) : 1;
        return v > 0 ? v : 1;
    }();
    return s;
}

static const xi_host_api& host_api() {
    static xi_host_api api = xi::ImagePool::make_host_api();
    return api;
}

// Owner thunks: read/write THIS thread's ImagePool owner slot — identical to
// service_main.cpp's owner_get_cb/owner_set_cb and test_parallel_safety.
static uint32_t test_owner_get() { return (uint32_t)xi::ImagePool::current_owner(); }
static void     test_owner_set(uint32_t id) {
    xi::ImagePool::current_owner_ref() = (xi::ImagePoolOwnerId)id;
}
static void wire_owner_thunks() {
    ::g_owner_get_fn_ = (void*)&test_owner_get;   // global-namespace thunk ptrs
    ::g_owner_set_fn_ = (void*)&test_owner_set;   // (xi_async.hpp uses ::g_owner_*)
}

// ===========================================================================
// Owner propagation under contention: T host threads, each its own owner epoch
// ===========================================================================

static void test_owner_propagation_stress() {
    SECTION("C1/C2/C3: many concurrent owner epochs — worker images attributed + swept");
    wire_owner_thunks();
    xi::clear_cancel();                 // parallel_for early-outs on a stale cancel
    const xi_host_api& host = host_api();
    auto& pool = xi::ImagePool::instance();

    constexpr int THREADS   = 6;
    const int     ITERS     = 150 * stress_scale();
    constexpr int PAR_IMGS  = 8;        // via parallel_for (omp workers)
    constexpr int ASYNC_IMGS = 4;       // via xi::async workers
    const int PER_ITER      = PAR_IMGS + ASYNC_IMGS;

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            for (int it = 0; it < ITERS; ++it) {
                // A fresh owner id per iteration → stats(P) counts ONLY this
                // iteration's images, even with the other threads churning.
                xi::ImagePoolOwnerId P = xi::ImagePool::alloc_owner_id();
                std::vector<xi_image_handle> par(PAR_IMGS, 0);
                std::vector<xi_image_handle> asy(ASYNC_IMGS, 0);
                {
                    xi::ImagePool::OwnerGuard g(P);   // this thread runs under P
                    xi::clear_cancel();
                    // Images created on omp worker threads must inherit P via the
                    // per-worker OwnerScope re-install inside parallel_for.
                    xi::parallel_for(PAR_IMGS, [&](int i) {
                        par[i] = host.image_create(4, 4, 1);
                    });
                    // ...and on xi::async workers via the copied owner scope.
                    std::vector<xi::Future<xi_image_handle>> fs;
                    for (int i = 0; i < ASYNC_IMGS; ++i)
                        fs.push_back(xi::async([&host]() -> xi_image_handle {
                            return host.image_create(4, 4, 1);
                        }));
                    for (int i = 0; i < ASYNC_IMGS; ++i) asy[i] = fs[i];   // await
                }
                for (auto h : par) CHECK(h != 0);
                for (auto h : asy) CHECK(h != 0);
                // Every worker-created image is attributed to P (not anonymous).
                CHECK(pool.stats(P).handle_count == PER_ITER);
                // P's sweep reclaims exactly them; other threads' owners untouched.
                int swept = pool.release_all_for(P);
                CHECK(swept == PER_ITER);
                CHECK(pool.stats(P).handle_count == 0);
            }
        });
    }
    for (auto& th : threads) th.join();

    // No image handle leaked across every owner epoch on every thread.
    CHECK(pool.stats().handle_count == 0);
}

// ===========================================================================
// Watchdog epoch-cancel: cross-thread semantics (in-flight targeted, late spared)
// ===========================================================================

static void test_watchdog_epoch_semantics() {
    SECTION("watchdog: in-flight inspects cancelled, post-trip inspects spared");
    const int REPS     = 40 * stress_scale();
    const int INFLIGHT = 8;

    for (int rep = 0; rep < REPS; ++rep) {
        xi::clear_cancel();

        std::atomic<int>  ready{0};
        std::atomic<bool> go{false};
        std::atomic<int>  inflight_saw_cancel{0};

        std::vector<std::thread> pool;
        for (int i = 0; i < INFLIGHT; ++i) {
            pool.emplace_back([&] {
                xi::begin_inspect();               // in-flight ticket (< cutoff)
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                // Cancel is now armed with a cutoff above our ticket → we're targeted.
                if (xi::cancellation_requested())
                    inflight_saw_cancel.fetch_add(1, std::memory_order_relaxed);
            });
        }
        while (ready.load(std::memory_order_acquire) < INFLIGHT) std::this_thread::yield();

        xi::arm_cancel();                          // cutoff = high-water > all tickets
        go.store(true, std::memory_order_release);
        for (auto& t : pool) t.join();
        CHECK(inflight_saw_cancel.load() == INFLIGHT);   // every in-flight inspect hit

        // Inspects that DRAW their ticket after the trip must not be cancelled by it.
        std::atomic<int> late_saw{0};
        std::vector<std::thread> late;
        for (int i = 0; i < INFLIGHT; ++i) {
            late.emplace_back([&] {
                xi::begin_inspect();               // ticket >= cutoff
                if (xi::cancellation_requested())
                    late_saw.fetch_add(1, std::memory_order_relaxed);
            });
        }
        for (auto& t : late) t.join();
        CHECK(late_saw.load() == 0);               // fresh inspects spared

        xi::clear_cancel();
    }
}

// ===========================================================================
// Watchdog epoch-cancel: high-contention arm/clear vs begin_inspect/poll
// ===========================================================================

static void test_watchdog_arm_clear_contention() {
    SECTION("watchdog: concurrent arm/clear vs begin/poll — ticket counter intact, no hang");
    xi::clear_cancel();

    const uint64_t initial = xi::cancel_ticket_counter().load(std::memory_order_relaxed);
    const int INSPECTORS = 6;
    const int ITERS      = 20'000 * stress_scale();
    std::atomic<bool>      stop{false};
    std::atomic<long long> total_begins{0};

    // Inspectors: draw a ticket and poll cancellation a few times, forever.
    std::vector<std::thread> inspectors;
    for (int t = 0; t < INSPECTORS; ++t) {
        inspectors.emplace_back([&] {
            long long begins = 0;
            while (!stop.load(std::memory_order_acquire)) {
                xi::begin_inspect();
                ++begins;
                // Poll the flag under whatever the watchdog is doing concurrently.
                for (int k = 0; k < 4; ++k) (void)xi::cancellation_requested();
            }
            total_begins.fetch_add(begins, std::memory_order_relaxed);
        });
    }

    // Watchdog: arm and clear in a tight loop, racing the inspectors' polls.
    std::thread watchdog([&] {
        for (int i = 0; i < ITERS; ++i) {
            xi::arm_cancel();
            std::this_thread::yield();
            xi::clear_cancel();
        }
    });
    watchdog.join();
    stop.store(true, std::memory_order_release);
    for (auto& t : inspectors) t.join();

    xi::clear_cancel();
    // The counter is the single source of truth for ticket identity. Under all
    // that contention, begin_inspect's fetch_add must have neither lost nor
    // duplicated a tick: final - initial == the number of begins we counted.
    const uint64_t final = xi::cancel_ticket_counter().load(std::memory_order_relaxed);
    CHECK((long long)(final - initial) == total_begins.load());
    CHECK(total_begins.load() > 0);
    std::printf("  %lld inspect tickets drawn under %d-way contention\n",
                total_begins.load(), INSPECTORS);
}

int main() {
    std::fprintf(stderr, "=== test_owner_cancel_stress (scale=%d) ===\n", stress_scale());
    auto t0 = std::chrono::steady_clock::now();

    test_owner_propagation_stress();
    test_watchdog_epoch_semantics();
    test_watchdog_arm_clear_contention();

    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    int f = g_failures.load();
    if (f == 0) {
        std::fprintf(stderr, "\nALL TESTS PASSED in %lld ms\n", (long long)dt);
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES (%lld ms)\n", f, (long long)dt);
    return 1;
}
