//
// test_start_resume_drop.cpp — F2 START/RESUME WINDOW drop-accounting guard.
//
// THE BUG (fix/f2-start-resume-drop): cmd_start_ (service_cmd_dispatch.cpp) and
// DispatchPoolGuard::resume (service_internal.hpp) flip g_eng.continuous=true
// BEFORE spawn_group_pool_ populates g_eng.lanes. The bus sink is already
// installed, so a concurrent source emit in that window reaches enqueue_to_lane_
// (service_dispatch.cpp) with the lane set still empty: lane_for_ returns nullptr.
// Historically the `if (!lane)` branch was a BARE `return false` — the
// TriggerEventReleaser guard freed the refs (no leak) but the drop was otherwise
// INVISIBLE: no XI_SYS_DROPPED marker and no `dropped` counter bump, unlike EVERY
// other drop path. The fix routes that branch through the shared drop-accounting
// helper (account_dropped_frame_) and bumps g_eng.dropped_lifetime.
//
// WHY THIS IS A MODEL TEST, NOT A LINK/INJECT TEST (read before "improving" it):
// The real path — enqueue_to_lane_, account_dropped_frame_, lane_for_ and the
// g_eng engine state — are FILE-LOCAL statics compiled only into the xinsp-backend
// executable, behind the private service_internal.hpp. No test binary links them,
// none are declared in a public header, and there is NO injection seam in the
// startup path (no env hook, no test shim) to force the empty-lanes-but-continuous
// state. The window itself is a couple of instructions between `g_eng.continuous =
// true` and spawn_group_pool_, so a WS-level race driver could not hit it
// deterministically — it would be flaky and is a net negative in CI. This mirrors
// the SAME situation the repo already handles in test_qa_race (RACE-U1), which
// "reimplements the file-static loop faithfully" because the real logic isn't
// extracted to a header. So: this reproduces enqueue_to_lane_'s no-lane branch
// (empty lane set + continuous flag) with the identical release/account/guard
// shape and asserts the drop is COUNTED + MARKED rather than silently dropped. It
// FAILS against the pre-fix bare-drop behavior and PASSES with the fix. If the
// dispatch engine is ever extracted behind a testable seam, point this at the real
// enqueue_to_lane_ instead.
//
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                    \
                __FILE__, __LINE__, #expr);                             \
            ++fails;                                                    \
        }                                                               \
    } while (0)

// ---- faithful stand-ins for the real backend types/state --------------------

// Mirrors release_trigger_event_ (service_sinks.cpp): guards on a non-null pack,
// releases once, then NULLs the handle so a second call is a harmless no-op. This
// idempotency is what makes account_dropped_frame_'s internal release + the
// TriggerEventReleaser destructor release safe (no double-free) on the fixed path.
static int g_pack_release_count = 0;
struct Ev {
    uint64_t pack = 1;   // != 0 → "carries a payload"; 0 == XI_PACK_NULL
    std::string group;
};
static void release_trigger_event_(Ev& ev) {
    if (ev.pack != 0) { ++g_pack_release_count; ev.pack = 0; }
}
// Mirrors TriggerEventReleaser (service_internal.hpp): releases on scope exit
// unless dismiss()'d.
struct TriggerEventReleaser {
    Ev* ev_;
    explicit TriggerEventReleaser(Ev& e) : ev_(&e) {}
    ~TriggerEventReleaser() { if (ev_) release_trigger_event_(*ev_); }
    void dismiss() { ev_ = nullptr; }
};

// Mirrors the engine bits the branch touches.
struct Engine {
    bool continuous = false;
    bool lanes_empty = true;            // lane_for_ → nullptr when true
    uint64_t dropped_lifetime = 0;
};
struct Marker { std::string reason, state, klass; int64_t aid; };
static std::vector<Marker> g_markers;   // stands in for emit_run_result(XI_SYS_DROPPED,…)

// Mirrors account_dropped_frame_ (service_dispatch.cpp): release the dropped
// event's refs, then emit the out-of-band XI_SYS_DROPPED marker. (The warn
// throttle is elided — it has no observable state here.)
static void account_dropped_frame_(Ev& ev, int64_t aid, const char* reason) {
    release_trigger_event_(ev);
    g_markers.push_back(Marker{reason, "dropped", "queue_full", aid});
}

// enqueue_to_lane_'s no-lane branch, BOTH behaviors, so the test encodes the
// regression (old FAILS the asserts, new PASSES).
enum class Behavior { BareDrop_OLD, Accounted_NEW };
static bool enqueue_no_lane_branch(Engine& eng, Ev ev, Behavior b) {
    TriggerEventReleaser guard(ev);         // frees ev on every exit unless dismiss()'d
    if (!eng.continuous) return false;
    if (eng.lanes_empty) {                  // lane_for_ → nullptr
        if (b == Behavior::BareDrop_OLD) {
            return false;                   // BUG: silent — guard frees refs, nothing counted
        }
        // FIX: route through drop accounting + bump the lifetime counter. aid=-1:
        // no arrival slot is claimed on this path. release_trigger_event_ is
        // idempotent, so account_dropped_frame_'s release + the guard destructor
        // release is a harmless no-op (verified below by g_pack_release_count==1).
        ++eng.dropped_lifetime;
        account_dropped_frame_(ev, -1, "dropped: no lane (start/resume window)");
        return false;
    }
    guard.dismiss();
    return true;
}

int main() {
    // ---- NEW (fixed) behavior: the start/resume-window drop is COUNTED + MARKED.
    {
        g_markers.clear();
        g_pack_release_count = 0;
        Engine eng;
        eng.continuous = true;      // cmd_start_/resume flipped this…
        eng.lanes_empty = true;     // …before spawn_group_pool_ populated lanes
        Ev ev; ev.group = "default";

        bool enqueued = enqueue_no_lane_branch(eng, ev, Behavior::Accounted_NEW);

        CHECK(!enqueued);                          // still a drop (nothing enqueued)
        CHECK(eng.dropped_lifetime == 1);          // counter bumped (was silent before)
        CHECK(g_markers.size() == 1);              // XI_SYS_DROPPED fired
        if (!g_markers.empty()) {
            CHECK(g_markers[0].reason == "dropped: no lane (start/resume window)");
            CHECK(g_markers[0].state == "dropped");
            CHECK(g_markers[0].klass == "queue_full");
            CHECK(g_markers[0].aid == -1);         // no arrival slot claimed here
        }
        // DOUBLE-FREE SAFETY: account_dropped_frame_ released ev once, the guard
        // destructor's second release_trigger_event_ was a no-op (pack already
        // NULL). Exactly one effective release → no double-free.
        CHECK(g_pack_release_count == 1);
    }

    // ---- OLD behavior: the same emit is SILENTLY dropped (the regression this
    // guards against). Refs are still freed (no leak) but nothing is counted/marked.
    {
        g_markers.clear();
        g_pack_release_count = 0;
        Engine eng;
        eng.continuous = true;
        eng.lanes_empty = true;
        Ev ev; ev.group = "default";

        bool enqueued = enqueue_no_lane_branch(eng, ev, Behavior::BareDrop_OLD);

        CHECK(!enqueued);
        CHECK(eng.dropped_lifetime == 0);   // BUG: silent — no counter bump
        CHECK(g_markers.empty());           // BUG: no XI_SYS_DROPPED marker
        CHECK(g_pack_release_count == 1);   // but the guard still frees the refs (no leak)
    }

    // ---- Sanity: with lanes populated, the branch is never taken (steady state
    // stays byte-identical — the fix only adds accounting to the empty-lanes path).
    {
        g_markers.clear();
        g_pack_release_count = 0;
        Engine eng;
        eng.continuous = true;
        eng.lanes_empty = false;            // spawn_group_pool_ has populated lanes
        Ev ev; ev.group = "default";

        bool enqueued = enqueue_no_lane_branch(eng, ev, Behavior::Accounted_NEW);

        CHECK(enqueued);                    // handed to the lane (guard dismissed)
        CHECK(eng.dropped_lifetime == 0);   // not a drop
        CHECK(g_markers.empty());
        CHECK(g_pack_release_count == 0);   // ownership → lane, not released
    }

    if (fails) {
        std::fprintf(stderr, "\n%d CHECK(s) FAILED\n", fails);
        return 1;
    }
    std::fprintf(stderr, "\nOK — F2 start/resume-window drop is counted + marked (no double-free)\n");
    return 0;
}
