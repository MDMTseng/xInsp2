//
// test_hot_reload_swap.cpp — concurrency probe for the hot-reload g_script swap
// (§28 Tier-2 GAP: "Hot-reload g_script swap — no dedicated test"). Drives the
// REAL loader (xi_script_loader.hpp) the way service_main.cpp does:
//
//   * readers snapshot the shared g_script BY VALUE under g_script_mu (copying its
//     module_lifetime shared_ptr), release the lock, then call inspect() OUTSIDE
//     the lock — exactly like run_one_inspection.
//   * a writer hot-reloads in a tight loop: load a fresh module, swap it into
//     g_script under the lock, and drop the old slot. The old module's owner-sweep
//     + FreeLibrary must be DEFERRED to module_lifetime's deleter so it fires only
//     after the last in-flight snapshot that referenced it has returned.
//
// If the snapshot discipline is broken (raw handle kept without the shared_ptr, or
// FreeLibrary run eagerly under the lock), an in-flight inspect() calls into a
// module that has been unmapped → the reload_probe magic check faults/aborts. To
// make FreeLibrary genuinely UNMAP (Windows refcounts a module by path, so
// reloading one path never unmaps), the writer cycles through N DISTINCT on-disk
// copies of the fixture — each truly unmaps when its last ref drops.
//
// No TSan on MSVC (OQ-1); run under -DXINSP2_SANITIZE=address to turn a stale-call
// UAF into an ASan report. XINSP2_STRESS_SCALE multiplies the reload count.
//

#include <xi/xi_script_loader.hpp>
#include <xi/xi_image_pool.hpp>

#include <filesystem>
#include <system_error>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef RELOAD_PROBE_DLL
#define RELOAD_PROBE_DLL "reload_probe.dll"
#endif

#define CHECK(expr)                                                  \
    do {                                                             \
        if (!(expr)) {                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                 \
                __FILE__, __LINE__, #expr);                          \
            std::abort();                                            \
        }                                                            \
    } while (0)

static int stress_scale() {
    static int s = [] {
        const char* e = std::getenv("XINSP2_STRESS_SCALE");
        int v = e ? std::atoi(e) : 1;
        return v > 0 ? v : 1;
    }();
    return s;
}

using xi::script::LoadedScript;

// The shared slot + its lock — the test's stand-in for g_script / g_script_mu.
static LoadedScript      g_script;
static std::mutex        g_script_mu;
static std::atomic<bool> g_keep_going{true};
static std::atomic<long long> g_calls{0};   // successful inspect() calls, all modules

// N distinct on-disk copies so a FreeLibrary of the last ref truly unmaps.
static constexpr int NCOPIES = 8;
static std::vector<std::string> g_copies;

static void make_copies() {
    namespace fs = std::filesystem;
    // Match the module suffix of the fixture we're copying (.dll / .so / .dylib).
    const std::string ext = fs::path(RELOAD_PROBE_DLL).extension().string();
    for (int i = 0; i < NCOPIES; ++i) {
        // Absolute path: POSIX dlopen only searches the cwd for a name that
        // contains a '/', so a bare "foo.so" would miss the copy we just wrote.
        std::string dst = (fs::current_path() /
                           ("reload_probe_copy_" + std::to_string(i) + ext)).string();
        // Overwrite any stale copy from a previous run.
        std::error_code ec;
        fs::copy_file(RELOAD_PROBE_DLL, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::fprintf(stderr, "FAIL: copy_file %s -> %s (%s)\n",
                         RELOAD_PROBE_DLL, dst.c_str(), ec.message().c_str());
            std::abort();
        }
        g_copies.push_back(dst);
    }
}

static void remove_copies() {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (auto& p : g_copies) fs::remove(p, ec);  // best-effort (may be mapped still)
}

// Load one distinct copy, bind the shared counter into it, swap it into g_script,
// and drop the previous slot. Returns false only on a genuine load failure.
static bool hot_reload(int which) {
    LoadedScript fresh;
    std::string err;
    if (!xi::script::load_script(g_copies[which % NCOPIES], fresh, err)) {
        std::fprintf(stderr, "load_script failed: %s\n", err.c_str());
        return false;
    }
    CHECK(fresh.ok());
    // Bind the shared success counter into the freshly-mapped image.
    auto bind = reinterpret_cast<void (*)(void*)>(
        GetProcAddress(fresh.handle, "reload_probe_bind"));
    CHECK(bind != nullptr);
    bind(&g_calls);

    LoadedScript old;
    {
        std::lock_guard<std::mutex> lk(g_script_mu);
        old = std::move(g_script);      // hand the old slot out of the shared spot
        g_script = std::move(fresh);    // publish the new one under the lock
    }
    // `old` drops its ref here, OUTSIDE the lock. If an in-flight reader still holds
    // a snapshot of it, module_lifetime keeps it mapped until that reader returns;
    // otherwise this is the last ref and the deleter unmaps it now.
    xi::script::unload_script(old);
    return true;
}

int main() {
    std::fprintf(stderr, "=== test_hot_reload_swap (scale=%d) ===\n", stress_scale());
    auto t0 = std::chrono::steady_clock::now();

    make_copies();

    // Seed g_script with the first module.
    CHECK(hot_reload(0));

    const int READERS = 8;
    const int RELOADS = 400 * stress_scale();
    std::atomic<long long> reader_calls{0};

    // Readers: snapshot-by-value under the lock, call inspect() outside it.
    std::vector<std::thread> readers;
    for (int r = 0; r < READERS; ++r) {
        readers.emplace_back([&, r] {
            long long local = 0;
            while (g_keep_going.load(std::memory_order_acquire)) {
                LoadedScript snap;
                {
                    std::lock_guard<std::mutex> lk(g_script_mu);
                    snap = g_script;    // copy => bumps module_lifetime refcount
                }
                if (snap.ok()) {
                    snap.inspect(r);    // OUTSIDE the lock; snap keeps it mapped
                    ++local;
                }
                // snap drops here; if it held the last ref to a retired module,
                // its deferred deleter unmaps it now — safely, call already done.
            }
            reader_calls.fetch_add(local, std::memory_order_relaxed);
        });
    }

    // Writer: hammer reloads through the distinct copies.
    long long reloads_done = 0;
    for (int i = 1; i <= RELOADS; ++i) {
        CHECK(hot_reload(i));
        ++reloads_done;
        if ((i & 15) == 0) std::this_thread::yield();
    }

    g_keep_going.store(false, std::memory_order_release);
    for (auto& t : readers) t.join();

    // Tear down the live slot — drops the last ref → deferred owner-sweep + unmap.
    xi::script::unload_script(g_script);

    // Correctness: every reader call was counted by the module it hit (magic
    // verified inside inspect on each), and the two counters agree.
    CHECK(reloads_done == RELOADS);
    CHECK(reader_calls.load() > 0);
    CHECK(g_calls.load() == reader_calls.load());
    // No image handles leaked across all the owner allocations/sweeps.
    CHECK(xi::ImagePool::instance().stats().handle_count == 0);

    remove_copies();

    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "reloads=%lld reader_calls=%lld sink=%lld\n",
                 reloads_done, reader_calls.load(), g_calls.load());
    std::fprintf(stderr, "\nALL TESTS PASSED in %lld ms\n", (long long)dt);
    return 0;
}
