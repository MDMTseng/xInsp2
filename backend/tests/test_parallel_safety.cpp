//
// test_parallel_safety.cpp — behavioral regression tests for the three
// parallel-region concurrency hazards fixed in Part I of the core fix plan
// (docs/roadmap/core-bug-hunt-2026-06-fix-plan.md companion + the W1 commits).
// These three hazards previously had ZERO test coverage.
//
//   B (OpenMP / SEH)  — xi::parallel_for must catch a hardware fault AND an
//                       ordinary std::exception INSIDE the omp region and
//                       rethrow on the calling thread, never terminating the
//                       process.  (xi_parallel.hpp)
//   C (image owner)   — an image created on a worker thread (via xi::async /
//                       xi::parallel_for) must inherit the PARENT inspect
//                       thread's image-pool owner, not anonymous (owner=0), so
//                       the per-owner leak sweep reclaims it.  (xi_async.hpp C2,
//                       xi_parallel.hpp C3, owner thunks C1)
//   A (trigger)       — xi::trigger_snapshot() captures the trigger BY VALUE on
//                       the inspect thread (addref'd images + frozen meta) so it
//                       can be read safely from ANY other thread, surviving the
//                       end of the originating dispatch.  (xi_use.hpp A2)
//
// Harness: the inline CHECK/SECTION style used by the neighboring test_*.cpp
// (test_xi_core / test_image_pool / test_golden_plugin), NOT xi_test.hpp —
// xi_test.hpp signals failure by THROWING, which is unsafe from inside the
// worker threads these tests assert on (and collides with the B test, which
// deliberately throws out of a parallel region). g_failures is atomic so a
// CHECK fired on a worker thread is race-free.
//
// Build: this target is compiled WITH /openmp (see backend/CMakeLists.txt) so
// the B + C tests exercise the REAL multi-threaded OpenMP path in
// xi::parallel_for, not the serial #else fallback. /EHa (set project-wide) is
// what makes _set_se_translator able to convert the SEH fault into a catchable
// xi::seh_exception.
//

#include <xi/xi.hpp>             // xi::async, xi::parallel_for, xi::seh_exception
#include <xi/xi_use.hpp>         // xi::trigger_snapshot / TriggerSnapshot
#include <xi/xi_image_pool.hpp>  // ImagePool + make_host_api + OwnerGuard
// THE CUT (v12): xi_doc_registry.hpp + the Record meta plane are gone. The A2/A4
// trigger tests now cover the IMAGE round-trip only (the meta-doc half was the
// deleted Record plane); B (SEH) and C (owner propagation) are unchanged.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ---- harness ---------------------------------------------------------------
static std::atomic<int> g_failures{0};
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures.fetch_add(1);                                          \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

// xi_use.hpp declares these as extern (the script DLL defines them static via
// xi_script_support.hpp). This unit test plays the role of the host: it OWNS
// the storage and points the trigger thunks at its own canned state. (The owner
// thunks g_owner_get_fn_/g_owner_set_fn_ are inline-defined in xi_async.hpp —
// we only assign them, below.)
void* g_use_process_fn_   = nullptr;
void* g_use_exchange_fn_  = nullptr;
void* g_use_grab_fn_      = nullptr;
void* g_use_host_api_     = nullptr;
void* g_trigger_info_fn_  = nullptr;
void* g_trigger_image_fn_ = nullptr;
void* g_trigger_sources_fn_ = nullptr;
void* g_trigger_leader_fn_  = nullptr;
void* g_trigger_meta_fn_    = nullptr;

// A single host_api over the live singleton ImagePool — exactly what the backend
// hands a script/plugin (image_* + doc_* all route to the real pool/registry).
static const xi_host_api& host_api() {
    static xi_host_api api = xi::ImagePool::make_host_api();
    return api;
}

// ===========================================================================
// B — xi::parallel_for SEH + std::exception containment + completeness
// ===========================================================================

// A runtime-null pointer the optimizer can't constant-fold away, so the store
// genuinely faults (access violation) instead of being elided as UB.
static int* volatile g_null_ptr = nullptr;

static void test_parallel_for_seh_fault_surfaces() {
    SECTION("B1: parallel_for surfaces an SEH hardware fault on the calling thread");

    std::atomic<int> ran{0};
    bool caught_seh = false;
    unsigned int code = 0;
    bool caught_other = false;
    try {
        xi::parallel_for(2000, [&](int i) {
            ran.fetch_add(1, std::memory_order_relaxed);
            if (i == 1234) {
                // Hardware fault on exactly one iteration. parallel_for installs
                // the SEH translator per omp worker, so this becomes a catchable
                // xi::seh_exception INSIDE the region (not a process kill).
                *g_null_ptr = 0xDEAD;
            }
        });
    } catch (const xi::seh_exception& e) {
        caught_seh = true;
        code = e.code;
    } catch (...) {
        caught_other = true;
    }
    // Reaching here at all proves the process was NOT terminated by the fault.
    CHECK(caught_seh);
    CHECK(!caught_other);
    CHECK(code == 0xC0000005u);   // EXCEPTION_ACCESS_VIOLATION
    // At least the faulting iteration ran; others may have drained early.
    CHECK(ran.load() >= 1);
    std::printf("  surfaced SEH code=0x%08X after %d iterations\n", code, ran.load());
}

static void test_parallel_for_cpp_exception_surfaces() {
    SECTION("B2: parallel_for surfaces a plain std::exception (the catch(...) gap fix)");

    bool caught = false;
    bool wrong = false;
    try {
        xi::parallel_for(256, [&](int i) {
            if (i == 7) throw std::runtime_error("body boom");
        });
    } catch (const std::runtime_error& e) {
        caught = (std::string(e.what()) == "body boom");
    } catch (...) {
        wrong = true;   // escaped as something else — the gap fix would be broken
    }
    CHECK(caught);
    CHECK(!wrong);
}

static void test_parallel_for_all_iterations_run() {
    SECTION("B3: a normal parallel_for body runs every iteration exactly once");

    constexpr int N = 4096;
    std::atomic<int> count{0};
    std::vector<int> seen(N, 0);
    xi::parallel_for(N, [&](int i) {
        count.fetch_add(1, std::memory_order_relaxed);
        seen[i] = 1;   // distinct index per iteration — no data race
    });
    CHECK(count.load() == N);
    bool all = true;
    for (int i = 0; i < N; ++i) if (!seen[i]) all = false;
    CHECK(all);
}

// ===========================================================================
// C — worker-thread image-pool owner propagation (async + parallel_for)
// ===========================================================================

// Owner thunks: identical to service_main.cpp's owner_get_cb/owner_set_cb —
// read/write THIS thread's ImagePool owner slot.
static uint32_t test_owner_get() { return (uint32_t)xi::ImagePool::current_owner(); }
static void     test_owner_set(uint32_t id) {
    xi::ImagePool::current_owner_ref() = (xi::ImagePoolOwnerId)id;
}

static void wire_owner_thunks() {
    g_owner_get_fn_ = (void*)&test_owner_get;
    g_owner_set_fn_ = (void*)&test_owner_set;
}

// F4: trigger-context marker thunks — identical to service_sinks.cpp's
// trigger_ctx_get_cb/set_cb: read/write THIS thread's marker slot. The host's
// warn_trigger_off_thread_ keys off exactly this per-thread value (fires iff
// != 0), so testing propagation of the value == testing the abort/no-abort
// decision it drives.
static thread_local int test_trigger_ctx_ = 0;
static uint32_t test_trigger_ctx_get() { return (uint32_t)test_trigger_ctx_; }
static void     test_trigger_ctx_set(uint32_t v) { test_trigger_ctx_ = (int)v; }
static void wire_trigger_ctx_thunks() {
    g_trigger_ctx_get_fn_ = (void*)&test_trigger_ctx_get;
    g_trigger_ctx_set_fn_ = (void*)&test_trigger_ctx_set;
}
// The exact predicate warn_trigger_off_thread_ evaluates on the CALLING thread:
// true ⇒ the host would fail loud (abort in debug / log-once in release); false ⇒
// the read is a legitimate "no trigger" and stays quiet.
static bool warn_would_fire_here() { return test_trigger_ctx_get() != 0; }

static void test_async_propagates_owner() {
    SECTION("C2: xi::async-created pool image is attributed to the PARENT owner");
    wire_owner_thunks();
    const xi_host_api& host = host_api();
    auto& pool = xi::ImagePool::instance();

    xi::ImagePoolOwnerId P = xi::ImagePool::alloc_owner_id();
    xi_image_handle h = 0;
    {
        // Inspect thread runs under owner P (as the dispatch worker does via
        // ImagePool::OwnerGuard around the script inspect).
        xi::ImagePool::OwnerGuard g(P);
        // The image is created on a DIFFERENT (worker) thread. Pre-fix it would
        // be tagged owner=0 (anonymous) and dropped from P's leak sweep.
        auto f = xi::async([&host]() -> xi_image_handle {
            return host.image_create(8, 8, 1);
        });
        h = f;   // await
    }
    CHECK(h != 0);
    // THE FIX: the worker-created image is attributed to P, not anonymous.
    // (P is a freshly alloc'd owner id, so stats(P) counts ONLY this image.)
    CHECK(pool.stats(P).handle_count == 1);   // pre-fix: 0 (it would be owner=0)

    // And P's sweep reclaims it (sole owner → genuine leak reclaimed).
    int swept = pool.release_all_for(P);
    CHECK(swept == 1);
    CHECK(pool.data(h) == nullptr);
}

static void test_parallel_for_propagates_owner() {
    SECTION("C3: xi::parallel_for-created pool images are attributed to the PARENT owner");
    wire_owner_thunks();
    const xi_host_api& host = host_api();
    auto& pool = xi::ImagePool::instance();

    constexpr int N = 8;
    xi::ImagePoolOwnerId P = xi::ImagePool::alloc_owner_id();
    std::vector<xi_image_handle> hs(N, 0);
    {
        xi::ImagePool::OwnerGuard g(P);
        xi::parallel_for(N, [&](int i) {
            hs[i] = host.image_create(4, 4, 1);   // on an omp worker thread
        });
    }
    for (int i = 0; i < N; ++i) CHECK(hs[i] != 0);
    // Every worker-created image inherited P (OwnerScope re-installs it per omp
    // worker inside the region). Pre-fix all N would be owner=0.
    CHECK(pool.stats(P).handle_count == N);

    int swept = pool.release_all_for(P);
    CHECK(swept == N);
    CHECK(pool.stats(P).handle_count == 0);
}

// ===========================================================================
// F4 — off-thread current_trigger() detection is RELATIONAL, not process-global
// ===========================================================================
//
// The old A1 heuristic stored the last CurrentTriggerScope's thread id in a single
// process-global atomic; warn_trigger_off_thread_ fired whenever it was != 0, WITHOUT
// comparing to the calling thread. Under max_parallel>1 + a live timer, lane A runs
// a TRIGGERED frame (stores tidA) while lane B runs a benign TIMER-TICK inspect and
// its script calls current_trigger() — B saw tidA != 0 and aborted a correct script
// (and B's scope storing 0 masked a genuine misuse on A). The fix makes the signal a
// per-thread marker propagated only into the framework's own worker threads. This
// test asserts the relational property the fix rests on — the same value
// warn_trigger_off_thread_ keys on:
//   * a triggered inspect's xi::async / xi::parallel_for CHILD reads marker 1
//     (genuine off-thread read ⇒ WOULD fire — detection preserved), and
//   * a timer-tick lane's own worker reads 0 (⇒ would NOT fire), CONCURRENTLY with a
//     triggered lane whose marker is 1 (the exact false-positive the old code hit).
static void test_off_thread_detection_is_relational() {
    SECTION("F4: off-thread current_trigger() detection is per-thread-relative, not a process-global tid");
    wire_trigger_ctx_thunks();

    // --- Lane A: an inspect thread INSIDE a trigger-bearing frame (CurrentTriggerScope
    //     sets marker=1 on the dispatch thread). ---
    test_trigger_ctx_ = 1;

    // A child spawned by xi::async from the triggered inspect inherits marker=1, so a
    // current_trigger() read on it is caught (genuine misuse — should snapshot).
    std::atomic<int> child_marker{-1};
    {
        auto f = xi::async([&child_marker]() {
            child_marker.store((int)test_trigger_ctx_get());
            return 0;
        });
        (void)(int)f;   // await
    }
    CHECK(child_marker.load() == 1);           // detection preserved for the real child

    // parallel_for body likewise inherits marker=1.
    std::atomic<int> par_saw_marker{0};
    std::atomic<int> par_iters{0};
    xi::parallel_for(64, [&](int) {
        par_iters.fetch_add(1);
        if (test_trigger_ctx_get() != 0) par_saw_marker.fetch_add(1);
    });
    CHECK(par_iters.load() == 64);
    CHECK(par_saw_marker.load() == 64);        // every worker inherited the trigger ctx

    // --- Lane B: a DIFFERENT lane's timer-tick worker, running CONCURRENTLY while lane
    //     A's marker is still 1. It was NOT spawned by lane A's async/parallel_for — it
    //     is a plain thread with its own thread_local marker (0). Pre-fix it would read
    //     the process-global tidA != 0 and abort a correct script. ---
    std::atomic<bool> lane_b_would_fire{true};
    std::thread lane_b([&]() {
        // No CurrentTriggerScope ran for THIS inspect (timer tick / plain cmd:run):
        // its marker is the fresh thread_local default, 0.
        lane_b_would_fire.store(warn_would_fire_here());
    });
    lane_b.join();
    CHECK(lane_b_would_fire.load() == false);  // THE FIX: benign lane-B worker is NOT aborted

    // --- The async worker restored the marker on exit — a pooled worker thread reused
    //     for a later, non-triggered task must not leak marker=1. ---
    std::atomic<int> reused_marker{-1};
    test_trigger_ctx_ = 0;                     // lane A's inspect ended (dtor clears)
    {
        auto f = xi::async([&reused_marker]() {
            reused_marker.store((int)test_trigger_ctx_get());
            return 0;
        });
        (void)(int)f;
    }
    CHECK(reused_marker.load() == 0);          // non-triggered inspect's child ⇒ quiet

    // Reset for neighbouring tests.
    test_trigger_ctx_ = 0;
}

// ===========================================================================
// A — xi::trigger_snapshot() cross-thread (A2)
// ===========================================================================
//
// We play the host: the trigger thunks read canned state below (mirroring the
// ref semantics of service_sinks.cpp's trigger_*_cb). The off-thread fail-loud's
// abort/log body (warn_trigger_off_thread_) lives in service_sinks.cpp and only
// runs with the real dispatch worker + CurrentTriggerScope; its DECISION INPUT —
// the per-thread trigger-context marker (F4) — is unit-tested above in
// test_off_thread_detection_is_relational (the marker == the value warn keys on).
// Here we test the unit-testable A2 surface: the snapshot round-trips correctly
// off-thread and keeps its images + meta alive past the end of the dispatch.

static bool                                         g_trig_active = false;
static std::unordered_map<std::string, xi_image_handle> g_trig_images;
static xi::CurrentTriggerInfo                       g_trig_info{};

static void test_trigger_info_fn(xi::CurrentTriggerInfo* out) {
    if (!out) return;
    if (!g_trig_active) { *out = xi::CurrentTriggerInfo{}; return; }
    *out = g_trig_info;
    out->is_active = 1;
}
static xi_image_handle test_trigger_image_fn(const char* source) {
    if (!g_trig_active || !source) return XI_IMAGE_NULL;
    auto it = g_trig_images.find(source);
    if (it == g_trig_images.end()) {
        if (g_trig_images.size() == 1) it = g_trig_images.begin();   // sole-image fallback
        else return XI_IMAGE_NULL;
    }
    host_api().image_addref(it->second);   // reserve a ref for the caller (== trigger_image_cb)
    return it->second;
}
static int32_t test_trigger_sources_fn(char* buf, int32_t buflen) {
    if (!g_trig_active || !buf) return 0;
    std::string out; bool first = true;
    for (auto& [s, h] : g_trig_images) { if (!first) out.push_back('\n'); first = false; out += s; }
    int32_t n = (int32_t)out.size();
    if (buflen < n + 1) return -n;
    std::memcpy(buf, out.data(), n); buf[n] = 0;
    return n;
}

static void test_trigger_snapshot_cross_thread() {
    SECTION("A2: trigger_snapshot round-trips cross-thread + outlives the dispatch");
    const xi_host_api& host = host_api();

    g_use_host_api_       = (void*)&host;
    g_trigger_info_fn_    = (void*)&test_trigger_info_fn;
    g_trigger_image_fn_   = (void*)&test_trigger_image_fn;
    g_trigger_sources_fn_ = (void*)&test_trigger_sources_fn;

    // --- host sets up an in-flight trigger: one image (the metadata doc rode the
    //     deleted Record plane — THE CUT removed it) ---
    xi_image_handle h = host.image_create(8, 8, 1);   // the "ev.images" ref (rc=1)
    {
        uint8_t* px = host.image_data(h);
        for (size_t i = 0; i < 64; ++i) px[i] = (uint8_t)(0x50 + (i & 0x0F));
    }
    g_trig_images["cam"] = h;

    g_trig_info = xi::CurrentTriggerInfo{};
    g_trig_info.id            = xi_trigger_id{0x1234ull, 0xABCDull};
    g_trig_info.timestamp_us  = 4242;
    g_trig_info.dequeued_at_us = 99;
    g_trig_active = true;

    // --- INSPECT THREAD: take the snapshot (reads thread_local host state) ---
    xi::TriggerSnapshot snap = xi::trigger_snapshot();
    CHECK(snap.is_active());
    CHECK(snap.sources().size() == 1);
    CHECK(snap.has_source("cam"));

    // --- Simulate the dispatch ENDING: the host drops its image ref and tears
    //     down the ambient trigger. The snapshot's own addref'd image ref must
    //     keep it alive on its own. ---
    host.image_release(h);     // image rc 2 -> 1 (only the snapshot holds it now)
    g_trig_active = false;
    g_trig_images.clear();

    // --- A DIFFERENT thread reads the snapshot. It touches NO thread_local and
    //     NO thunk — everything is by-value/addref'd into the snapshot. ---
    std::thread worker([&]() {
        // id / timestamps round-trip
        CHECK(snap.id().hi == 0x1234ull);
        CHECK(snap.id().lo == 0xABCDull);
        CHECK(snap.timestamp_us() == 4242);
        CHECK(snap.dequeued_at_us() == 99);

        // image: still valid, correct geometry, sentinel pixels intact (the
        // held addref kept the pool entry alive past host.image_release above)
        xi::Image img = snap.image("cam");
        CHECK(!img.empty());
        CHECK(img.width == 8 && img.height == 8 && img.channels == 1);
        const uint8_t* px = img.data();
        CHECK(px != nullptr);
        bool sentinel_ok = (px != nullptr);
        if (px) for (size_t i = 0; i < 64; ++i)
            if (px[i] != (uint8_t)(0x50 + (i & 0x0F))) { sentinel_ok = false; break; }
        CHECK(sentinel_ok);
        // sole-image fallback: any key resolves a single-image snapshot
        CHECK(!snap.image("anything-else").empty());
    });
    worker.join();

    // --- Drop the snapshot: its held image ref is the LAST, so the pool image
    //     is now reclaimed (no leak, no double-free). ---
    {
        xi::TriggerSnapshot dead = std::move(snap);
        (void)dead;   // dead dies here
    }
    CHECK(xi::ImagePool::instance().data(h) == nullptr);          // image reclaimed
}

// ===========================================================================
// A4 — explicit-trigger entry: xi_trigger_view -> xi::Trigger is self-contained
// ===========================================================================
//
// The A4 root cure passes the trigger EXPLICITLY: the host fills a C-ABI
// xi_trigger_view and the SDK (inside xi_inspect_entry_tv) builds a xi::Trigger
// from it. Unlike the ambient current_trigger() path, that Trigger touches NO
// thread_local and NO thunk — so it is valid on ANY thread and safe to capture
// by value into a parallel body, and survives past the end of the originating
// dispatch (its own addref'd image ref + frozen-meta ref keep both alive). This
// is the same guarantee A2's trigger_snapshot gives, but delivered by the entry
// itself so the script never reaches for the ambient trigger at all.

static void test_explicit_trigger_view_cross_thread() {
    SECTION("A4: xi_trigger_view -> xi::Trigger self-contained + cross-thread safe");
    const xi_host_api& host = host_api();

    // --- host sets up an in-flight trigger: one image (the metadata doc rode the
    //     deleted Record plane — THE CUT removed it) ---
    xi_image_handle h = host.image_create(8, 8, 1);
    { uint8_t* px = host.image_data(h); for (size_t i = 0; i < 64; ++i) px[i] = (uint8_t)(0xA0 + (i & 0x0F)); }

    // --- host fills the explicit view (borrowed handle) and the SDK constructs
    //     the Trigger from it — exactly what run_one_inspection +
    //     xi_inspect_entry_tv do ---
    xi_trigger_view_image imgs[1] = { { "cam", h } };
    xi_trigger_view view{};
    view.is_active      = 1;
    view.id             = xi_trigger_id{ 0x11ull, 0x22ull };
    view.timestamp_us   = 777;
    view.dequeued_at_us = 42;
    view.images         = imgs;
    view.image_count    = 1;
    view.leader_source  = "cam";
    view.host           = &host;

    xi::Trigger t(&view);
    CHECK(t.is_active());
    CHECK(t.id().hi == 0x11ull && t.id().lo == 0x22ull);
    CHECK(t.timestamp_us() == 777);
    CHECK(t.dequeued_at_us() == 42);
    CHECK(t.primary_source() == "cam");
    CHECK(t.has_source("cam"));
    CHECK(t.sources().size() == 1);

    // --- dispatch ENDS: host drops its image ref. The Trigger's own addref'd
    //     image ref must keep it alive. ---
    host.image_release(h);   // rc 2 -> 1 (only the Trigger holds it now)

    // --- a DIFFERENT thread reads a BY-VALUE copy (as a parallel body would). No
    //     thread_local, no thunk — everything came in through the explicit view. ---
    xi::Trigger tcopy = t;   // cheap shared_ptr copy
    std::thread worker([&]() {
        xi::Image img = tcopy.image("cam");
        CHECK(!img.empty());
        CHECK(img.width == 8 && img.height == 8 && img.channels == 1);
        const uint8_t* px = img.data();
        bool sentinel_ok = (px != nullptr);
        if (px) for (size_t i = 0; i < 64; ++i)
            if (px[i] != (uint8_t)(0xA0 + (i & 0x0F))) { sentinel_ok = false; break; }
        CHECK(sentinel_ok);
        CHECK(!tcopy.image("anything-else").empty());   // sole-image fallback
    });
    worker.join();

    // --- drop both Trigger copies: their held image ref is the LAST, so the pool
    //     image is reclaimed (no leak, no double-free). ---
    { xi::Trigger a = std::move(t); xi::Trigger b = std::move(tcopy); (void)a; (void)b; }
    CHECK(xi::ImagePool::instance().data(h) == nullptr);          // image reclaimed
}

// An INACTIVE view (plain cmd:run / timer tick: g_current_trigger == nullptr on
// the host) must yield an inactive Trigger — no crash, mirrors the old path's
// current_trigger().is_active() == false.
static void test_explicit_trigger_view_inactive() {
    SECTION("A4: an inactive/empty xi_trigger_view yields an inactive Trigger");
    xi::Trigger none(nullptr);
    CHECK(!none.is_active());
    CHECK(none.image("cam").empty());
    CHECK(none.sources().empty());

    xi_trigger_view view{};   // is_active=0, host=null
    xi::Trigger t(&view);
    CHECK(!t.is_active());
    CHECK(t.image("cam").empty());
}

// ===========================================================================

int main() {
    // B — most self-contained; do thoroughly.
    test_parallel_for_seh_fault_surfaces();
    test_parallel_for_cpp_exception_surfaces();
    test_parallel_for_all_iterations_run();

    // C — owner propagation onto worker threads.
    test_async_propagates_owner();
    test_parallel_for_propagates_owner();

    // F4 — off-thread current_trigger() detection is per-thread-relative (no
    // process-global tid that false-aborts a benign timer-tick worker under N>1).
    test_off_thread_detection_is_relational();

    // A — trigger snapshot cross-thread (A2). A1 fail-loud is integration-level.
    test_trigger_snapshot_cross_thread();

    // A4 — explicit-trigger entry: the host-filled view builds a self-contained
    // Trigger that is safe off-thread and outlives the dispatch.
    test_explicit_trigger_view_cross_thread();
    test_explicit_trigger_view_inactive();

    int f = g_failures.load();
    if (f == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", f);
    return 1;
}
