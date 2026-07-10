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
#include <xi/xi_result.hpp>      // xi::result / xi::ng (A4 run-context routing test)
#include <xi/xi_thread.hpp>      // xi::spawn_worker (A4 context propagation test)
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
// A4 explicit per-run context thunks (this TU plays the host — see xi_io.hpp /
// xi_result.hpp externs). g_run_ctx_get_fn_/set_fn_ are inline globals in
// xi_async.hpp (we only assign them, in wire_run_ctx_thunks below).
void* g_run_ctx_run_id_fn_     = nullptr;   // xi_io.hpp (long long(*)())
void* g_run_ctx_frame_path_fn_ = nullptr;   // xi_io.hpp (const char*(*)())
void* g_result_fn_             = nullptr;   // xi_result.hpp (void(*)(int,const char*))

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

// A4 explicit per-run context — this TU mirrors the backend's host-side RunContext
// + run_ctx thunks (service_result.cpp): a per-thread installed context pointer,
// get/set to marshal it across the spawn primitives, and run_id/frame_path/result
// reads that resolve it. Testing propagation of THIS context == testing that
// xi::run_id()/current_frame_path()/xi::result() are correct on a worker thread.
struct TestRunResult { int code = 0; std::string msg; bool set = false; };
struct TestRunCtx {
    long long       run_id = 0;
    std::string     frame_path;
    TestRunResult*  result_slot = nullptr;   // the RUN's verdict slot (parent thread)
};
static thread_local const TestRunCtx* t_run_ctx = nullptr;   // installed context (null ⇒ off-run)

// get/set: the propagation thunks the spawn primitives use (opaque pointer).
static const void* test_run_ctx_get() { return t_run_ctx; }
static void        test_run_ctx_set(const void* p) { t_run_ctx = static_cast<const TestRunCtx*>(p); }
// run_id/frame_path: the read thunks xi::run_id()/current_frame_path() call.
static long long   test_run_ctx_run_id() { return t_run_ctx ? t_run_ctx->run_id : 0; }
static const char* test_run_ctx_frame_path() { return t_run_ctx ? t_run_ctx->frame_path.c_str() : ""; }
// result routing: xi::result() → this thunk → the RUN's slot via the context, so a
// verdict set on a worker lands in the parent run's slot (not a per-thread copy).
static void test_result(int code, const char* msg) {
    if (!t_run_ctx || !t_run_ctx->result_slot) return;
    t_run_ctx->result_slot->code = code;
    t_run_ctx->result_slot->msg  = msg ? msg : "";
    t_run_ctx->result_slot->set  = true;
}
// spawn_worker BY-VALUE snapshot thunks (mirror run_ctx_snapshot_cb /
// run_ctx_install_worker_cb / run_ctx_free_cb): a worker-OWNED heap copy with the
// verdict slot dropped, so a spawn_worker that outlives its inspect reads the
// snapshot (no dangling pointer into the freed parent context) and routes no verdict.
static void* test_run_ctx_snapshot() {
    if (!t_run_ctx) return nullptr;
    auto* s = new TestRunCtx(*t_run_ctx);
    s->result_slot = nullptr;               // detached: no live run slot to route into
    return s;
}
static void test_run_ctx_install_worker(void* s) { t_run_ctx = static_cast<const TestRunCtx*>(s); }
static void test_run_ctx_free(void* s) { delete static_cast<TestRunCtx*>(s); }

static void wire_run_ctx_thunks() {
    g_run_ctx_get_fn_             = (void*)&test_run_ctx_get;
    g_run_ctx_set_fn_             = (void*)&test_run_ctx_set;
    g_run_ctx_run_id_fn_         = (void*)&test_run_ctx_run_id;
    g_run_ctx_frame_path_fn_     = (void*)&test_run_ctx_frame_path;
    g_run_ctx_snapshot_fn_       = (void*)&test_run_ctx_snapshot;
    g_run_ctx_install_worker_fn_ = (void*)&test_run_ctx_install_worker;
    g_run_ctx_free_fn_           = (void*)&test_run_ctx_free;
    g_result_fn_                 = (void*)&test_result;
}
// RAII install on the "dispatch" thread (mirrors the backend's RunContextScope).
struct TestRunCtxScope {
    const TestRunCtx* prev;
    explicit TestRunCtxScope(const TestRunCtx* c) : prev(t_run_ctx) { t_run_ctx = c; }
    ~TestRunCtxScope() { t_run_ctx = prev; }
};

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
// A4 — explicit per-run context propagates into worker threads (the spawn-gap
//      closure): run_id() / current_frame_path() / result() are correct on any
//      xi::async / xi::parallel_for / xi::spawn_worker worker.
// ===========================================================================
//
// The pre-A4 bug: run_id / frame_path lived in an ambient thread_local set on the
// dispatch thread and NEVER propagated to workers — so a xi::run_id() /
// xi::current_frame_path() read from a parallel body silently returned 0/"", and a
// xi::result() from a worker wrote a per-thread copy the run never read (silent NG
// loss). A4 rides ONE explicit context installed on the dispatch thread and
// captured-by-value into the spawn primitives. This test installs the context (as
// run_inspection_compute_'s RunContextScope does) and asserts every worker reads
// the RIGHT run values and routes its verdict to the run's slot.
static void test_run_ctx_propagates_into_workers() {
    SECTION("A4: run_id()/current_frame_path()/result() correct inside parallel_for + async (spawn-gap closure)");
    wire_run_ctx_thunks();

    // Off any run: no context installed → accessors read the sentinel (in Debug the
    // real backend thunk would abort; the TEST thunk returns 0/"" so we can assert
    // the wiring without aborting the test process).
    t_run_ctx = nullptr;
    CHECK(xi::run_id() == 0);
    CHECK(xi::current_frame_path().empty());

    // --- Install the run's context on THIS (dispatch) thread. ---
    TestRunResult verdict;
    TestRunCtx ctx;
    ctx.run_id      = 4242;
    ctx.frame_path  = "C:/frames/f0007.png";
    ctx.result_slot = &verdict;
    TestRunCtxScope scope(&ctx);

    // On the dispatch thread the accessors read the run.
    CHECK(xi::run_id() == 4242);
    CHECK(xi::current_frame_path() == "C:/frames/f0007.png");

    // --- xi::async worker: run_id / frame_path / result all resolve to the run. ---
    std::atomic<long long> async_run_id{-1};
    bool async_path_ok = false;
    {
        auto f = xi::async([&]() -> int {
            async_run_id.store(xi::run_id());
            // capture frame_path off-thread + route a verdict from the worker
            std::string fp = xi::current_frame_path();
            async_path_ok = (fp == "C:/frames/f0007.png");
            xi::ng(2, "defect from async worker");   // → test_result → run's slot
            return 0;
        });
        (void)(int)f;   // await
    }
    CHECK(async_run_id.load() == 4242);        // pre-A4: 0 (ambient TLS not propagated)
    CHECK(async_path_ok);                      // pre-A4: ""
    CHECK(verdict.set);                        // pre-A4: worker wrote a lost per-thread copy
    CHECK(verdict.code == -2);                 // xi::ng(2) routed to the RUN's slot
    CHECK(verdict.msg == "defect from async worker");

    // --- xi::parallel_for body: every worker reads the right run_id + frame_path. ---
    verdict = TestRunResult{};
    constexpr int N = 64;
    std::atomic<int> id_ok{0}, path_ok{0}, iters{0};
    xi::parallel_for(N, [&](int) {
        iters.fetch_add(1);
        if (xi::run_id() == 4242) id_ok.fetch_add(1);
        if (xi::current_frame_path() == "C:/frames/f0007.png") path_ok.fetch_add(1);
    });
    CHECK(iters.load() == N);
    CHECK(id_ok.load() == N);                  // every worker saw the run id (gap closed)
    CHECK(path_ok.load() == N);                // …and the frame_path

    // A verdict set after the join, on the dispatch thread, also routes correctly.
    xi::ok(1, "clean");
    CHECK(verdict.set && verdict.code == 1 && verdict.msg == "clean");

    // --- xi::spawn_worker also inherits the context (was OwnerScope-only before). ---
    std::atomic<long long> worker_run_id{-1};
    {
        std::thread th = xi::spawn_worker("ctx-probe", [&]() {
            worker_run_id.store(xi::run_id());
        });
        th.join();
    }
    CHECK(worker_run_id.load() == 4242);

    // --- Scope exit restores "off-run": a later async task (pooled thread reused)
    //     must not leak the previous run's context. ---
    // (scope dtor runs at end of function; assert the pooled-thread reuse here while
    //  the context is still installed vs after — do the after-check post-scope below.)
}

// THE spawn_worker UAF regression: a spawn_worker thread that OUTLIVES its inspect
// must read its own by-value SNAPSHOT (no dangling pointer into the freed dispatch
// frame), and a verdict from it must safely no-op. We spawn a worker that BLOCKS on
// a gate, let the RunContextScope (and the stack `ctx`) destruct, THEN release the
// gate so the worker's whole body runs after the parent context is gone. If
// spawn_worker still captured a raw pointer into `ctx`, reading run_id() here would
// be a use-after-free (garbage / crash under ASAN) — this is the test the earlier
// "don't read after the inspect" caveat lacked.
static void test_spawn_worker_outlives_inspect() {
    SECTION("A4: a spawn_worker OUTLIVING its inspect reads its snapshot safely (no UAF); result() no-ops");
    wire_run_ctx_thunks();

    auto gate = std::make_shared<std::atomic<bool>>(false);
    std::atomic<long long> wid{-1};
    std::atomic<int>       path_ok{-1};
    std::atomic<bool>      body_done{false};
    TestRunResult          verdict;   // the run's slot — must stay UNSET (the worker's snapshot has none)
    std::thread th;
    {
        TestRunCtx ctx;
        ctx.run_id      = 7;
        ctx.frame_path  = "C:/frames/late.png";
        ctx.result_slot = &verdict;
        TestRunCtxScope scope(&ctx);
        // Snapshot is taken HERE (spawning thread, context live). The worker blocks
        // until we release the gate AFTER this scope + `ctx` have destructed.
        th = xi::spawn_worker("late-worker",
            [gate, &wid, &path_ok, &body_done]() {
                while (!gate->load(std::memory_order_acquire)) std::this_thread::yield();
                wid.store(xi::run_id());                                  // snapshot → 7 (ctx is gone)
                path_ok.store(xi::current_frame_path() == "C:/frames/late.png" ? 1 : 0);
                xi::ok(1, "verdict from a detached worker");              // result_slot null → no-op
                body_done.store(true);
            });
    }   // <-- RunContextScope + ctx destruct HERE, worker still alive & blocked on the gate

    gate->store(true, std::memory_order_release);   // release: the whole body runs post-teardown
    th.join();

    CHECK(body_done.load());
    CHECK(wid.load() == 7);                 // read the worker-OWNED snapshot, not the freed ctx (no UAF)
    CHECK(path_ok.load() == 1);
    CHECK(!verdict.set);                    // result() from the detached worker safely no-op'd
}

// After the run's scope has ended, a fresh async task sees no context (the worker
// thread the pool reuses must not retain the previous run's pointer).
static void test_run_ctx_cleared_after_run() {
    SECTION("A4: a worker of a later task does not leak the previous run's context");
    wire_run_ctx_thunks();
    t_run_ctx = nullptr;                        // no run installed
    std::atomic<long long> leaked{-1};
    {
        auto f = xi::async([&]() -> int { leaked.store(xi::run_id()); return 0; });
        (void)(int)f;
    }
    CHECK(leaked.load() == 0);                  // no stale context propagated
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
    view.run_id         = 909;                 // A4 explicit per-run context on the view
    view.frame_path     = "C:/frames/x.png";

    xi::Trigger t(&view);
    CHECK(t.is_active());
    CHECK(t.id().hi == 0x11ull && t.id().lo == 0x22ull);
    CHECK(t.timestamp_us() == 777);
    CHECK(t.dequeued_at_us() == 42);
    CHECK(t.run_id() == 909);                  // self-contained per-run context
    CHECK(t.frame_path() == "C:/frames/x.png");
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
        CHECK(tcopy.run_id() == 909);                    // per-run context rides the copy
        CHECK(tcopy.frame_path() == "C:/frames/x.png");
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

    // A4 — explicit per-run context propagates into worker threads: run_id() /
    // current_frame_path() / result() are correct inside parallel_for + async
    // (the spawn-gap closure), and don't leak across runs.
    test_run_ctx_propagates_into_workers();
    test_spawn_worker_outlives_inspect();
    test_run_ctx_cleared_after_run();

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
