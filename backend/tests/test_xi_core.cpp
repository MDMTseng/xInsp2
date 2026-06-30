//
// test_xi_core.cpp — M0 regression test (assertion-based).
//
// Covers every primitive in xi/xi.hpp with real assertions. Fails the build
// on regression. Header-only, no dependencies beyond STL.
//

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <xi/xi.hpp>
#include <xi/xi_inflight_runs.hpp>
#include <xi/xi_working_copy.hpp>
#include <xi/xi_emit_gate.hpp>

// Minimal test harness — each TEST() runs once; failures print and set a flag.
static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define SECTION(name) std::printf("[test] %s\n", name)

// ---------- xi_async ----------

static int slow_add(int a, int b, int delay_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    return a + b;
}

static int always_throws(int) {
    throw std::runtime_error("boom");
}

static void test_async_basic() {
    SECTION("async basic");
    auto f = xi::async(slow_add, 2, 3, 5);
    int v  = f;  // implicit await
    CHECK(v == 5);
}

static void test_async_parallel() {
    SECTION("async parallel wall time");
    using namespace std::chrono;
    auto t0 = steady_clock::now();
    auto a  = xi::async(slow_add, 1, 1, 50);
    auto b  = xi::async(slow_add, 2, 2, 50);
    auto c  = xi::async(slow_add, 3, 3, 50);
    int  ra = a, rb = b, rc = c;
    auto dt = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    CHECK(ra == 2);
    CHECK(rb == 4);
    CHECK(rc == 6);
    // Three 50ms tasks in parallel should finish in < 150ms (sequential)
    // and typically around 55-80ms. Allow generous slack for CI.
    std::printf("  parallel wall time: %lldms\n", (long long)dt);
    CHECK(dt < 130);
}

static void test_async_exception() {
    SECTION("async exception propagation");
    auto f = xi::async(always_throws, 42);
    bool caught = false;
    try {
        int v = f;  // implicit await should re-throw
        (void)v;
    } catch (const std::runtime_error& e) {
        caught = (std::string(e.what()) == "boom");
    }
    CHECK(caught);
}

static int square(int x) { return x * x; }
ASYNC_WRAP(square)

static void test_async_cancel_cooperative() {
    SECTION("async cancel — cooperative task observes the flag");
    // Long-running task that polls cancellation_requested() at a
    // 1ms loop. We expect it to exit early (well under the 500ms it
    // would otherwise take) once we call cancel().
    std::atomic<bool> task_started{false};
    std::atomic<int>  iterations{0};
    auto f = xi::async([&] {
        task_started.store(true);
        for (int i = 0; i < 500; ++i) {
            if (xi::cancellation_requested()) return -1;
            iterations.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 999;
    });
    while (!task_started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    f.cancel();
    int rc = f;
    CHECK(rc == -1);
    CHECK(iterations.load() < 100);   // cancel observed early; 500 means it never noticed
}

static void test_async_cancel_idempotent() {
    SECTION("async cancel — idempotent + survives task without cancellation API");
    auto f = xi::async([] { return 42; });
    int v = f;       // task already returned; cancel() after the fact must be safe
    f.cancel();
    f.cancel();
    CHECK(v == 42);
    CHECK(f.cancelled());
}

static void test_watchdog_cancel_epoch_scope() {
    SECTION("watchdog cancel — epoch-scoped: fresh inspect after a trip is NOT cancelled");
    // Models the dispatch thread reusing one OS thread frame-after-frame.
    // Inspect A starts (draws a ticket), then the watchdog trips while A is in
    // flight, then a FRESH inspect B starts on the same thread. A must observe
    // the cancel; B (started AFTER the trip) must NOT — that fresh-frame-poison
    // was core-bug-hunt #12.
    xi::clear_cancel();
    CHECK(!xi::cancellation_requested());          // nothing armed

    uint64_t a = xi::begin_inspect();              // inspect A starts
    (void)a;
    xi::arm_cancel();                              // watchdog trips with A in flight
    CHECK(xi::cancellation_requested());           // A is targeted → observes cancel

    uint64_t b = xi::begin_inspect();              // fresh inspect B starts (same thread)
    (void)b;
    CHECK(b > a);                                  // strictly-increasing ticket
    CHECK(!xi::cancellation_requested());          // THE FIX: B is not poisoned

    // Escalation preserved: if B *itself* later overruns, a SECOND trip targets
    // it (cutoff now above B's ticket) and B does observe the cancel.
    xi::arm_cancel();
    CHECK(xi::cancellation_requested());

    // Clearing the cancel releases everyone.
    xi::clear_cancel();
    CHECK(!xi::cancellation_requested());
}

static void test_watchdog_cancel_epoch_async_propagates() {
    SECTION("watchdog cancel — epoch scope propagates into xi::async sub-tasks");
    // A sub-task spawned by an in-flight (targeted) inspect must see the cancel;
    // a sub-task spawned by a fresh post-trip inspect must not. Proves the
    // ticket rides into the worker thread (whose own thread_local would be 0).
    xi::clear_cancel();

    (void)xi::begin_inspect();                     // inspect A
    xi::arm_cancel();                              // trip while A in flight
    bool sub_a_cancelled = xi::async([] {
        return xi::cancellation_requested();
    });
    CHECK(sub_a_cancelled);                        // A's sub-task is targeted

    (void)xi::begin_inspect();                     // fresh inspect B (post-trip)
    bool sub_b_cancelled = xi::async([] {
        return xi::cancellation_requested();
    });
    CHECK(!sub_b_cancelled);                        // B's sub-task is not poisoned

    xi::clear_cancel();
    CHECK(!xi::cancellation_requested());
}

static void test_async_wrap() {
    SECTION("ASYNC_WRAP");
    auto f    = async_square(9);
    int  v    = f;
    CHECK(v == 81);
}

static std::atomic<int> side_effect{0};
static void bump() { side_effect.fetch_add(1); }

static void test_await_all_mixed_void() {
    SECTION("await_all accepts Future<void> + non-void, void is awaited but filtered out");
    side_effect.store(0);
    auto fv1 = xi::async(bump);
    auto fi  = xi::async([](){ return 7; });
    auto fv2 = xi::async(bump);
    auto fd  = xi::async([](){ return 3.5; });

    auto t = xi::await_all(fv1, fi, fv2, fd);
    static_assert(std::tuple_size_v<decltype(t)> == 2,
                  "void futures must contribute no tuple entry");
    CHECK(std::get<0>(t) == 7);
    CHECK(std::get<1>(t) > 3.4 && std::get<1>(t) < 3.6);
    CHECK(side_effect.load() == 2);
}

// ---------- xi_param ----------

static void test_param_basic() {
    SECTION("Param implicit read + clamp");
    xi::Param<double> sigma{"test_sigma", 3.0, {0.1, 10.0}};
    double v = sigma;
    CHECK(v == 3.0);

    sigma.set(20.0);  // out of range, clamps to 10
    CHECK(static_cast<double>(sigma) == 10.0);
    sigma.set(-5.0);
    CHECK(static_cast<double>(sigma) == 0.1);

    // Registry lookup
    auto* found = xi::ParamRegistry::instance().find("test_sigma");
    CHECK(found != nullptr);
    CHECK(found->name() == "test_sigma");
    CHECK(found->type_name() == "float");

    // JSON round-trip via set_from_json
    CHECK(found->set_from_json("7.5"));
    CHECK(static_cast<double>(sigma) == 7.5);
    CHECK(!found->set_from_json("not_a_number"));
}

static void test_param_bool() {
    SECTION("Param<bool>");
    xi::Param<bool> flag{"test_flag", false};
    CHECK(static_cast<bool>(flag) == false);
    auto* p = xi::ParamRegistry::instance().find("test_flag");
    CHECK(p != nullptr);
    CHECK(p->set_from_json("true"));
    CHECK(static_cast<bool>(flag) == true);
    CHECK(!p->set_from_json("maybe"));
}

// ---------- xi_instance ----------

class DummyPlugin : public xi::InstanceBase {
public:
    explicit DummyPlugin(std::string n) : name_(std::move(n)) {}
    const std::string& name() const override { return name_; }
    std::string plugin_name() const override { return "DummyPlugin"; }
    int counter = 0;
private:
    std::string name_;
};

namespace xi {
template <>
std::shared_ptr<DummyPlugin> make_plugin_instance<DummyPlugin>(std::string_view name) {
    return std::make_shared<DummyPlugin>(std::string(name));
}
}

static void test_instance_basic() {
    SECTION("Instance<T> create + registry");
    xi::InstanceRegistry::instance().clear();
    xi::Instance<DummyPlugin> a{"plugin_a"};
    CHECK(a);
    CHECK(a->name() == "plugin_a");
    a->counter = 42;

    // Same name → reuses existing
    xi::Instance<DummyPlugin> a2{"plugin_a"};
    CHECK(a2);
    CHECK(a2->counter == 42);

    auto list = xi::InstanceRegistry::instance().list();
    CHECK(list.size() == 1);
    CHECK(list[0]->plugin_name() == "DummyPlugin");
}

// ---------- main ----------

// ---- InflightRuns: the detached-run lifetime owner (shutdown-window UAF class) ----
static void test_inflight_runs() {
    SECTION("InflightRuns launch / drain / shutdown-bail");
    xi::InflightRuns rt;

    // A launched run holds the in-flight count until it returns; drain() waits it
    // out. This is what teardown relies on so srv outlives every detached run.
    std::atomic<bool> release{false};
    std::atomic<int>  ran{0};
    bool ok = rt.launch([&]{
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ran.fetch_add(1);
    });
    CHECK(ok);
    // Busy-wait until the thread is actually in flight (count observed == 1).
    for (int i = 0; rt.inflight() == 0 && i < 1000; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(rt.inflight() == 1);
    release.store(true);
    CHECK(rt.drain(5000));            // drains to zero within the cap
    CHECK(rt.inflight() == 0);
    CHECK(ran.load() == 1);

    // Reversible pause (lifecycle quiesce): while paused launch() bails like a
    // shutdown, but unpause() restores it — and it NESTS (a counter), unlike the
    // terminal begin_shutdown. This is what lets a lifecycle op refuse one-shots
    // while it FreeLibrary's a DLL, then re-enable them.
    rt.pause();
    CHECK(!rt.launch([]{}));          // paused → refused
    CHECK(rt.inflight() == 0);        // bail leaked no count
    rt.pause();                       // nest a second level
    rt.unpause();                     // back to one level — still paused
    CHECK(!rt.launch([]{}));
    rt.unpause();                     // fully unpaused
    std::atomic<int> ran3{0};
    bool ok3 = rt.launch([&]{ ran3.fetch_add(1); });
    CHECK(ok3);                       // runs again
    CHECK(rt.drain(5000));
    CHECK(ran3.load() == 1);

    // After begin_shutdown(), launch() must BAIL (run nothing, return false) so a
    // late source emit can't start a run against an about-to-die srv.
    rt.begin_shutdown();
    CHECK(rt.shutting_down());
    std::atomic<int> ran2{0};
    bool ok2 = rt.launch([&]{ ran2.fetch_add(1); });
    CHECK(!ok2);                      // refused
    CHECK(rt.inflight() == 0);        // no count leaked by the bail
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(ran2.load() == 0);          // fn never ran
    CHECK(rt.drain(100));             // drain is trivially satisfied
}

// ---- working-copy exclusion: precise, not "any component named X" ----
static void test_wc_exclusion() {
    SECTION("xi::wc::is_excluded is root-anchored / plugins-scoped");
    using xi::wc::is_excluded;
    namespace fs = std::filesystem;
    // Root-anchored infra: excluded.
    CHECK(is_excluded(fs::path(".xinsp_work") / "project.json"));
    CHECK(is_excluded(fs::path(".git") / "config"));
    CHECK(is_excluded(fs::path(".xinsp_commit_pending")));
    // Plugin CMake build output: excluded (only under plugins/).
    CHECK(is_excluded(fs::path("plugins") / "det" / "build" / "det.dll"));
    // USER data that merely shares a name must be PRESERVED (the silent-loss bug):
    CHECK(!is_excluded(fs::path("instances") / "cam0" / "build" / "asset.bin"));
    CHECK(!is_excluded(fs::path("build") / "out"));              // a root build/ that isn't a plugin's
    CHECK(!is_excluded(fs::path("assets") / ".git"));            // coincidental nested name
    CHECK(!is_excluded(fs::path("plugins") / "det" / "src" / "plugin.cpp"));
    CHECK(!is_excluded(fs::path("inspect.cpp")));
}

// ---- EmitTurn: ordered emit + early-return backstop (the lane-deadlock guard) ----
static void test_emit_gate() {
    SECTION("EmitTurn orders emits + dtor backstops an early return");
    using xi::EmitGate; using xi::EmitTurn;

    // (1) Out-of-order completion is serialized to ARRIVAL order: seq 1 starts first
    //     and blocks; seq 0 emits, then seq 1 unblocks and emits second.
    EmitGate g;
    std::atomic<int> tick{0};
    int e0 = -1, e1 = -1;
    std::thread th([&]() {
        EmitTurn t1(&g, 1, nullptr);   // nullptr keep_going = never "stop"
        t1.wait_turn();                // blocks until gate.next == 1
        e1 = tick.fetch_add(1);
        t1.complete();
    });
    // Let the seq-1 thread reach its wait before seq 0 runs.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    {
        EmitTurn t0(&g, 0, nullptr);
        t0.wait_turn();                // gate.next == 0 → immediate
        e0 = tick.fetch_add(1);
        t0.complete();                 // advance → 1, wakes the seq-1 thread
    }
    th.join();
    CHECK(e0 == 0);                     // seq 0 emitted first...
    CHECK(e1 == 1);                     // ...seq 1 second, despite starting earlier
    CHECK(g.next == 2);

    // (2) Early-return backstop: a turn that NEVER wait_turn/complete's (an early
    //     return before the emit) must still advance the cursor from its dtor, or the
    //     next seq deadlocks. This is the bug the restructure fixes.
    EmitGate g2;
    { EmitTurn t0(&g2, 0, nullptr); /* simulate early return: do nothing */ }
    CHECK(g2.next == 1);               // dtor took turn 0 + advanced
    { EmitTurn t1(&g2, 1, nullptr); t1.wait_turn(); t1.complete(); }
    CHECK(g2.next == 2);

    // (3) Completion mode (seq < 0) is a total no-op. wait_turn() reports "our turn"
    //     (true) so an ungated cmd:run still flushes its staged sinks.
    EmitGate g3;
    { EmitTurn tn(&g3, -1, nullptr); CHECK(tn.wait_turn() == true); tn.complete(); }
    CHECK(g3.next == 0);

    // (4) wait_turn() VERDICT (the $seq/ordered-sink fix): on its genuine turn it
    //     returns true; if the lane STOPS before its turn (keep_going flips false, which
    //     wakes every parked seq at once) it returns false, so a caller doing an ordered
    //     side effect (flushing staged sink deliveries) skips it instead of firing out of
    //     order. seq 0 with next==0 is immediately our turn.
    EmitGate g4;
    CHECK((EmitTurn(&g4, 0, nullptr).wait_turn()) == true);   // next==seq → our turn
    // A later seq parks (next==0, not its turn); flipping keep_going false wakes it with
    // next still 0, so it is NOT its turn → false.
    EmitGate g5;
    std::atomic<bool> keep_going{true};
    bool verdict = true;
    std::thread sth([&]() {
        EmitTurn t5(&g5, 5, &keep_going);   // waits for next==5 OR stop
        verdict = t5.wait_turn();
        t5.complete();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));   // let it park
    keep_going.store(false);
    { std::lock_guard<std::mutex> lk(g5.mu); g5.cv.notify_all(); }   // mirror stop_group_pool_
    sth.join();
    CHECK(verdict == false);            // woke for the stop, not its turn → skip the flush
    CHECK(g5.next == 0);               // complete() did NOT advance (next != seq)
}

int main() {
    test_inflight_runs();
    test_wc_exclusion();
    test_emit_gate();
    test_async_basic();
    test_async_parallel();
    test_async_exception();
    test_async_wrap();
    test_async_cancel_cooperative();
    test_async_cancel_idempotent();
    test_watchdog_cancel_epoch_scope();
    test_watchdog_cancel_epoch_async_propagates();

    test_await_all_mixed_void();

    test_param_basic();
    test_param_bool();

    test_instance_basic();

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
        return 1;
    }
}
