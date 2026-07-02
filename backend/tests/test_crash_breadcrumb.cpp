//
// test_crash_breadcrumb.cpp — crash-breadcrumb slot RECYCLING (concurrency-review
// finding 3 / adoption map item 11). Each thread claims a fixed breadcrumb slot
// (xi::crash::ctx()) and must RELEASE it on thread exit, so a long-running
// deployment that churns thousands of short-lived dispatch threads never exhausts
// the 64 slots and never degrades to the racy shared fallback slot 0.
//
// Pre-fix: a slot was claimed against an empty tid and never reset, so after 64
// LIFETIME threads every later inspect fell through to slot 0 — collapsing crash
// attribution. This test drives >>64 lifetime threads at bounded concurrency (the
// exact one-shot-dispatch pattern) and asserts (a) each worker gets a UNIQUELY
// attributed slot (never the shared fallback), and (b) live slot count returns to
// baseline after every batch joins — i.e. slots are reclaimed, not leaked.
//

#include <xi/xi_crash_dump.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
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

// Count breadcrumb slots currently claimed (non-zero owning tid).
static int live_slots() {
    int n = 0;
    for (int i = 0; i < xi::crash::kMaxSlots; ++i)
        if (xi::crash::g_slot_tid[i].load(std::memory_order_acquire) != 0) ++n;
    return n;
}

// One short-lived dispatch-like worker: claim a slot, stamp a breadcrumb, and
// verify the slot is UNIQUELY attributed to this thread. If recycling were broken,
// after 64 lifetime threads ctx() would hand back the shared fallback slot 0 whose
// thread_id belongs to an earlier thread -> the tid check below fails.
static void worker_body() {
    auto& c = xi::crash::ctx();
    xi::crash::set(c.last_plugin, sizeof(c.last_plugin), "churn");
    xi::crash::set_phase("inspect");
#ifdef _WIN32
    uint32_t tid = (uint32_t)GetCurrentThreadId();
    CHECK(c.thread_id == tid);   // real slot, not the shared fallback
#endif
}

int main() {
    const int baseline = live_slots();   // whatever the main thread already holds

    // 400 lifetime threads, at most `per` alive at once — far exceeds kMaxSlots
    // (64) over the run, but never exceeds it concurrently. This is the churn the
    // dispatch model produces (a fresh detached thread per non-continuous frame).
    const int batches = 50, per = 8;
    for (int b = 0; b < batches; ++b) {
        std::vector<std::thread> ts;
        ts.reserve(per);
        for (int i = 0; i < per; ++i) ts.emplace_back(worker_body);
        for (auto& t : ts) t.join();
        // join() completes only after the worker's thread_local SlotGuard dtor has
        // run, so its slot must be back in the pool: live count == baseline.
        CHECK(live_slots() == baseline);
    }

    // Recycling held across all 400 lifetime threads: no slot leak, no fallback.
    CHECK(g_failures.load() == 0);
    CHECK(live_slots() == baseline);

    if (g_failures.load() == 0) {
        std::fprintf(stderr, "test_crash_breadcrumb: ALL PASS (churned %d threads, "
                             "%d slots, no leak)\n", batches * per, xi::crash::kMaxSlots);
        return 0;
    }
    std::fprintf(stderr, "test_crash_breadcrumb: %d FAIL\n", g_failures.load());
    return 1;
}
