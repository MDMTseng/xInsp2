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
#include <xi/xi_instance.hpp>   // xi::Instance<T> / InstanceBase (no longer in the xi.hpp umbrella)
#include <xi/xi_inflight_runs.hpp>
#include <xi/xi_working_copy.hpp>
#include <xi/xi_emit_gate.hpp>
#include <xi/xi_trigger_bus.hpp>

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

// Read a whole file into a string (test helper; "" if absent/unreadable).
static std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

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

    // T1: drain() must return FALSE when a run stays in flight past the timeout.
    // This is the exact signal controlled_shutdown_teardown_ acts on — a wedged
    // inspect that won't drain makes drain() report failure, and teardown then
    // hard-exits (std::_Exit(WATCHDOG_EXIT_CODE)) rather than proceed into the
    // close_project/FreeLibrary + srv-destroy that would UAF against the live run.
    {
        xi::InflightRuns wedged;
        std::atomic<bool> stuck_release{false};
        std::atomic<int>  stuck_ran{0};
        bool okw = wedged.launch([&]{
            while (!stuck_release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            stuck_ran.fetch_add(1);
        });
        CHECK(okw);
        for (int i = 0; wedged.inflight() == 0 && i < 1000; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(wedged.inflight() == 1);
        CHECK(!wedged.drain(100));         // still held past the cap → reports failure
        CHECK(wedged.inflight() == 1);     // and the run really is still stuck
        stuck_release.store(true);         // control: once freed a normal drain succeeds
        CHECK(wedged.drain(5000));         // (so the false above wasn't spurious)
        CHECK(wedged.inflight() == 0);
        CHECK(stuck_ran.load() == 1);
    }

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

    // B1: max-in-flight cap. Fire K >> cap blocking runs; count must stay <= cap
    // and every over-cap launch must report drop-newest (dropped_over_cap=true,
    // returns false) — this is the bound that stops a fast source from spawning
    // unbounded detached threads / pinned frames.
    {
        xi::InflightRuns capped;
        capped.set_cap(4);
        CHECK(capped.cap() == 4);
        capped.set_cap(0);                                     // 0 → default, not unlimited
        CHECK(capped.cap() == xi::InflightRuns::kDefaultCap);
        capped.set_cap(4);
        std::atomic<bool> hold{true};
        std::atomic<int>  launched{0}, dropped{0};
        for (int i = 0; i < 64; ++i) {
            bool over = false;
            bool ok = capped.launch([&]{
                while (hold.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }, &over);
            if (ok)        ++launched;
            if (over)      ++dropped;   // over-cap → drop-newest
            CHECK(capped.inflight() <= 4);    // never exceeds the cap
        }
        CHECK(launched.load() <= 4);
        CHECK(dropped.load() >= 60);          // the rest were dropped-newest
        CHECK(capped.inflight() <= 4);
        hold.store(false);
        CHECK(capped.drain(5000));
        CHECK(capped.inflight() == 0);
    }

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

// ---- working-copy seed: a torn copy must NOT become a committable scratch ----
// BUG: open_project(working_copy=true) used to DISCARD copy_tree_excluding's
// bool; a copy that failed deep (disk full / locked file) but happened to land
// project.json passed the exists() guard and became authoritative. The user
// edits it, commit's mirror_tree then PRUNES the canonical files that merely
// failed to copy — silent data loss. The fix: seed_working_copy() honours the
// copy result, tears the partial scratch DOWN, and returns false so open aborts.
static void test_wc_seed_fail() {
    SECTION("xi::wc seed honours copy failure; torn scratch never authoritative");
    namespace fs = std::filesystem;
    std::error_code ec;
    // Unique temp root so the test is hermetic + re-runnable.
    fs::path base = fs::temp_directory_path() /
                    ("xi_wc_seed_" + std::to_string((unsigned long long)
                        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(base, ec);

    // --- (A) copy_tree_excluding reports false on a TORN copy, and the torn tree
    //     can still carry project.json — exactly the dangerous state the old guard
    //     accepted. Portable fault: pre-stage dst/data as a regular FILE so copying
    //     the source's data/ directory onto it fails, while project.json copies. ---
    {
        fs::path canon = base / "canon";
        fs::create_directories(canon / "data", ec);
        { std::ofstream(canon / "project.json") << "{\"name\":\"p\"}"; }
        { std::ofstream(canon / "data" / "big.bin") << "payload"; }
        fs::path dst = base / "dst_torn";
        fs::create_directories(dst, ec);
        { std::ofstream(dst / "data") << "I am a file blocking the dir"; }  // collision
        bool ok = xi::wc::copy_tree_excluding(canon, dst);
        CHECK(ok == false);                               // copy was torn → reported
        CHECK(fs::exists(dst / "project.json"));          // ...yet project.json landed
        // ^ the precise torn-but-passes-exists() state the seed wrapper must reject.
    }

    // --- (B) seed_working_copy: FAILED seed → false AND no committable scratch
    //     left behind. Portable fault: target the scratch under a path component
    //     that is a regular FILE, so the scratch can never be created. ---
    {
        fs::path canon = base / "canon2";
        fs::create_directories(canon, ec);
        { std::ofstream(canon / "project.json") << "{\"name\":\"p\"}"; }
        fs::path blocker = base / "blocker";            // a FILE, not a dir
        { std::ofstream(blocker) << "x"; }
        fs::path scratch = blocker / ".xinsp_work";     // uncreatable (parent is a file)
        bool ok = xi::wc::seed_working_copy(canon, scratch);
        CHECK(ok == false);                              // seed aborted
        CHECK(!fs::exists(scratch / "project.json"));    // nothing committable left
    }

    // --- (C) successful seed → true, scratch fully populated (no regression). ---
    {
        fs::path canon = base / "canon3";
        fs::create_directories(canon / "instances" / "cam0", ec);
        { std::ofstream(canon / "project.json") << "{\"name\":\"p\"}"; }
        { std::ofstream(canon / "inspect.cpp") << "// script"; }
        { std::ofstream(canon / "instances" / "cam0" / "def.json") << "{}"; }
        fs::path scratch = canon / ".xinsp_work";
        bool ok = xi::wc::seed_working_copy(canon, scratch);
        CHECK(ok == true);
        CHECK(fs::exists(scratch / "project.json"));
        CHECK(fs::exists(scratch / "inspect.cpp"));
        CHECK(fs::exists(scratch / "instances" / "cam0" / "def.json"));
    }

    fs::remove_all(base, ec);
}

// ---- working-copy discard must not destroy crash recovery (bug #14) ----------
// BUG: reopen_fresh_working_copy() (the Discard handler) did close → remove_all
// (scratch) → reopen, with NO check of the commit-pending marker. If a prior
// commit was interrupted (kCommitMarker present + canonical left torn/partial),
// the intact scratch is the ONLY source that can heal the canonical. Removing it
// before open_project's roll-forward ran left the torn canonical permanently
// unrecoverable. The fix factors the heal into xi::wc::roll_forward_pending_commit
// and Discard calls it BEFORE remove_all, completing the interrupted commit.
static void test_wc_discard_completes_pending_commit() {
    SECTION("Discard heals a torn canonical from scratch before dropping it");
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path base = fs::temp_directory_path() /
                    ("xi_wc_rf_" + std::to_string((unsigned long long)
                        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(base, ec);

    // Replicate the EXACT Discard sequence (reopen_fresh_working_copy): heal a
    // pending commit from the scratch FIRST, and only drop the scratch if the heal
    // succeeded. The load-bearing call is the production helper.
    auto discard_like = [](const fs::path& canon) {
        std::error_code e;
        if (xi::wc::roll_forward_pending_commit(canon))
            fs::remove_all(canon / ".xinsp_work", e);
    };

    // --- (A) WITH a pending marker: an interrupted commit left the canonical TORN
    //     (old project.json bytes, a committed file missing) while the scratch is a
    //     COMPLETE snapshot of the new state. Discard must COMPLETE the commit
    //     (heal canonical from scratch), not destroy the only recovery source. ---
    {
        fs::path canon = base / "canonA";
        fs::create_directories(canon, ec);
        // Torn canonical: stale project.json, and the new file never made it in.
        { std::ofstream(canon / "project.json") << "{\"name\":\"OLD-torn\"}"; }
        // Intact scratch (the snapshot the interrupted commit was mirroring FROM).
        fs::path scratch = canon / ".xinsp_work";
        fs::create_directories(scratch / "instances" / "cam0", ec);
        { std::ofstream(scratch / "project.json") << "{\"name\":\"NEW-complete\"}"; }
        { std::ofstream(scratch / "inspect.cpp") << "// new script"; }
        { std::ofstream(scratch / "instances" / "cam0" / "def.json") << "{\"v\":1}"; }
        // Plant the commit-pending journal marker (commit was interrupted).
        { std::ofstream(canon / ".xinsp_commit_pending") << "commit in progress\n"; }

        discard_like(canon);

        // Canonical is HEALED to the scratch's complete content (commit completed).
        CHECK(read_file(canon / "project.json") == "{\"name\":\"NEW-complete\"}");
        CHECK(fs::exists(canon / "inspect.cpp"));                       // new file restored
        CHECK(fs::exists(canon / "instances" / "cam0" / "def.json"));  // ...no data lost
        // Marker cleared (commit no longer pending) and scratch dropped (discard done).
        CHECK(!fs::exists(canon / ".xinsp_commit_pending"));
        CHECK(!fs::exists(scratch));
    }

    // --- (B) NO pending marker (the normal case): Discard just drops the scratch;
    //     the canonical is untouched. No regression — heal is a no-op. ---
    {
        fs::path canon = base / "canonB";
        fs::create_directories(canon, ec);
        { std::ofstream(canon / "project.json") << "{\"name\":\"canonical\"}"; }
        fs::path scratch = canon / ".xinsp_work";
        fs::create_directories(scratch, ec);
        { std::ofstream(scratch / "project.json") << "{\"name\":\"uncommitted-edit\"}"; }
        // No .xinsp_commit_pending marker.

        discard_like(canon);

        CHECK(read_file(canon / "project.json") == "{\"name\":\"canonical\"}");  // untouched
        CHECK(!fs::exists(scratch));                                              // edits dropped
    }

    fs::remove_all(base, ec);
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

// ---------- TriggerBus per-source emit-time map pruning (bug #17) ----------

static void test_trigger_bus_reset_prunes_source_map() {
    SECTION("TriggerBus::reset() prunes the per-source emit-time map");
    auto& bus = xi::TriggerBus::instance();
    // Start from a clean, sink-less bus so emit() takes the no-sink path
    // (which releases the images it addref'd) and nothing fires elsewhere.
    bus.clear_sink();
    bus.reset();
    CHECK(bus.source_emit_ages_us().empty());

    // One real image handle is enough to drive emit() past its image_count
    // guard; emit() addrefs it and the no-sink path releases that ref.
    xi_image_handle h = xi::ImagePool::instance().create(2, 2, 1);
    xi_record_image ri{};
    ri.handle = h;
    ri.key    = nullptr;

    const int N = 64;
    for (int i = 0; i < N; ++i) {
        std::string src = "cam_" + std::to_string(i);
        bus.emit(src, xi_trigger_id{0, 0}, /*ts*/0, &ri, /*image_count*/1,
                 /*meta_doc*/nullptr);
    }
    // Every distinct source name left a permanent entry pre-fix.
    CHECK(bus.source_emit_ages_us().size() == static_cast<size_t>(N));

    bus.reset();
    // Post-fix: reset() prunes the map. (Pre-fix / gated no-op: stays N -> FAIL.)
    CHECK(bus.source_emit_ages_us().empty());

    xi::ImagePool::instance().release(h);   // drop our own create() ref
}

int main() {
    test_trigger_bus_reset_prunes_source_map();
    test_inflight_runs();
    test_wc_exclusion();
    test_wc_seed_fail();
    test_wc_discard_completes_pending_commit();
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
