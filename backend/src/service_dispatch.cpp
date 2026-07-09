//
// service_dispatch.cpp — per-group dispatch lanes, pool lifecycle, trigger sink,
// controlled teardown + host-tracked instance state. Split from service_main.cpp
// (behavior-preserving; see service_internal.hpp).
//
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#  include <sys/resource.h>   // setpriority / PRIO_PROCESS (process-priority port)
#  include <cerrno>
#endif

#include "service_internal.hpp"

using xi::EmitGate;
using xi::EmitTurn;

// ---- Dispatch groups: per-group worker lanes (gated on parallelism.groups) ----
// Each group owns its own queue + max_parallel worker threads at its OS
// thread_priority, draining only its own queue. Only active when the project
// declares groups; with no groups a single synthesized default lane runs everything (the old separate single pool is gone — see lane_for_()).
// Result ordering is per-lane completion order in v1 (per-group arrival + the
// `group` wire tag are follow-ups). See docs/internals/dispatch.md.
// P1-8: lifetime-cumulative dispatch counters. The per-lane dropped/high_watermark
// reset on every cmd:start (lanes are recreated by spawn_group_pool_), which erases
// the "how much did we drop last run" history — a restart looks clean. These
// process-globals persist for the whole backend uptime (like ImagePool's
// total_created_), so a monitor can see total drops / peak depth across run
// boundaries. dispatch_stats reports them as *_lifetime alongside the per-run ones.

// GroupLane struct definition moved to service_internal.hpp.
// Lanes are shared_ptr + guarded by g_eng.lanes_mu so a producer (an emit thread /
// the timer) that grabbed a lane can't have it destroyed under it by a concurrent
// stop_group_pool_ — the shared_ptr keeps the GroupLane (its mutex/cv) alive until
// the producer is done. Fixes the lane-lifetime UAF found in v1 hardening.
// F4: default_group as captured WHEN the current lane set was spawned (guarded by
// g_eng.lanes_mu, set in spawn_group_pool_). lane_for_ routes against THIS, not a live
// model read — so routing can never reference a group name that isn't in g_eng.lanes.
// Today groups are load-only (open_project quiesces + respawns), so the live value
// can't diverge from the lanes; reading the snapshot makes that self-consistent by
// construction instead of by that external invariant — the safe shape for when a
// runtime "reconfigure groups" command is eventually added.

static void set_os_thread_priority_(const std::string& p) {
#ifdef _WIN32
    int pr = THREAD_PRIORITY_NORMAL;
    if      (p == "high") pr = THREAD_PRIORITY_ABOVE_NORMAL;
    else if (p == "low")  pr = THREAD_PRIORITY_BELOW_NORMAL;
    SetThreadPriority(GetCurrentThread(), pr);
#else
    (void)p;   // TODO(linux): pthread_setschedprio / nice
#endif
}

// Pin the current thread to a set of cores (a MASK — the thread may run on ANY of
// them, not just one). Empty `cores` = leave unbound. Bogus core ids are dropped
// (intersected with the process's allowed mask) so a bad config can't wipe the
// affinity to nothing.
static void set_os_thread_affinity_(const std::vector<int>& cores) {
    if (cores.empty()) return;
#ifdef _WIN32
    DWORD_PTR want = 0;
    for (int c : cores) if (c >= 0 && c < 64) want |= (DWORD_PTR(1) << c);  // 64-bit mask (≤64 cores; >64 = processor groups, TODO)
    if (!want) return;
    DWORD_PTR procMask = 0, sysMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask)) want &= procMask;
    if (want) SetThreadAffinityMask(GetCurrentThread(), want);
#else
    (void)cores;   // TODO(linux): cpu_set_t + pthread_setaffinity_np / sched_setaffinity
#endif
}

// Set the backend PROCESS priority class (Win). Returns true if applied. Used at
// startup (--priority) and live (cmd:set_process_priority). "" = leave unchanged.
bool apply_process_priority_(const std::string& cls) {
    if (cls.empty()) return false;
#ifdef _WIN32
    DWORD c = 0;
    if      (cls == "high")     c = HIGH_PRIORITY_CLASS;
    else if (cls == "above")    c = ABOVE_NORMAL_PRIORITY_CLASS;
    else if (cls == "normal")   c = NORMAL_PRIORITY_CLASS;
    else if (cls == "below")    c = BELOW_NORMAL_PRIORITY_CLASS;
    else if (cls == "realtime") c = REALTIME_PRIORITY_CLASS;
    else return false;
    SetPriorityClass(GetCurrentProcess(), c);
    std::fprintf(stderr, "[xinsp2] process priority = %s\n", cls.c_str());
    return true;
#else
    // POSIX: map the priority class onto a process nice value (setpriority).
    // Lowering priority (positive nice) always works; RAISING it (negative nice)
    // needs CAP_SYS_NICE, which a normal deploy may lack — so a failed raise is
    // logged best-effort but the (valid) class is still reported applied, rather
    // than surfacing as "bad priority class". ("realtime" is approximated by the
    // lowest nice, not SCHED_FIFO, to avoid requiring privileged RT scheduling.)
    int nice_val;
    if      (cls == "high")     nice_val = -10;
    else if (cls == "above")    nice_val = -5;
    else if (cls == "normal")   nice_val = 0;
    else if (cls == "below")    nice_val = 5;
    else if (cls == "realtime") nice_val = -20;
    else return false;
    errno = 0;
    if (::setpriority(PRIO_PROCESS, 0, nice_val) != 0 && errno != 0) {
        std::fprintf(stderr, "[xinsp2] process priority '%s' (nice %d) not applied: %s "
                     "(needs CAP_SYS_NICE to raise) — best-effort\n",
                     cls.c_str(), nice_val, std::strerror(errno));
    } else {
        std::fprintf(stderr, "[xinsp2] process priority = %s (nice %d)\n", cls.c_str(), nice_val);
    }
    return true;
#endif
}

// Warn (once per start) if the total dispatch worker count exceeds the core count.
// Oversubscription causes context-switch thrash that usually slows inspects — a
// dedicated inspection PC should keep Σ max_parallel ≤ cores (minus a couple for
// the FE supervisor / OS).
static void warn_oversubscribe_(int total_workers) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw > 0 && total_workers > (int)hw)
        std::fprintf(stderr,
            "[xinsp2] WARNING: %d dispatch worker threads on %u cores (oversubscribed) — "
            "context-switch thrash may slow inspects; lower max_parallel or add cores\n",
            total_workers, hw);
}

// Resolve a group name to its lane (holding g_eng.lanes_mu). Unknown/typo'd group →
// the default_group lane; if that too is absent, the first lane as a last resort.
// That front() floor is unreachable in today's load-only group model — a synthesized
// default lane always matches default_group_snapshot (F4 in docs/roadmap/known-issues.md)
// — so it exists only as a defensive floor for a future runtime "reconfigure groups"
// path. It is NOT silent: the first time it fires we log loudly (reaching it means
// routing hit a group the live lane set never knew about).
// Returns a shared_ptr so the caller keeps the lane alive past a concurrent stop.
static std::shared_ptr<GroupLane> lane_for_(const std::string& group) {
    std::lock_guard<std::mutex> lk(g_eng.lanes_mu);
    if (g_eng.lanes.empty()) return nullptr;
    for (auto& l : g_eng.lanes) if (l->cfg.name == group) return l;
    const std::string& dg = g_eng.default_group_snapshot;   // F4: spawn-time snapshot, not a live read
    if (!dg.empty()) for (auto& l : g_eng.lanes) if (l->cfg.name == dg) return l;
    static std::atomic<bool> warned_front{false};
    if (!warned_front.exchange(true))
        std::fprintf(stderr,
            "[xinsp2] WARN lane_for_ fell back to the first lane for group='%s' "
            "(no group match and no default_group lane) — routing to '%s'\n",
            group.empty() ? "(default)" : group.c_str(),
            g_eng.lanes.front()->cfg.name.c_str());
    return g_eng.lanes.front();
}

// Frame drops are reported to a connected client via emit_run_result, but an
// unattended factory PC with no UI would degrade SILENTLY (be_log stays clean
// while the system throws away work). Leave a throttled breadcrumb in stderr too.
// Powers-of-2 throttle: 1,2,4,8,… — visible from the first drop, never spammy.
static void warn_frame_drop_(uint64_t dropped, const std::string& group, const char* policy) {
    if (dropped == 0) return;
    if (dropped == 1 || (dropped & (dropped - 1)) == 0)
        std::fprintf(stderr,
            "[xinsp2] WARN dropping frames (group='%s', policy=%s, dropped=%llu) — source out-running processing\n",
            group.empty() ? "(default)" : group.c_str(), policy, (unsigned long long)dropped);
}

// Shared drop accounting for the two back-pressure paths that discard a frame:
// the continuous lane's drop_newest branch (enqueue_to_lane_) and the one-shot
// max_inflight cap (dispatch_one_shot_). Both re-derived the identical tail:
// release the dropped event's image + doc refs, warn (throttled), then emit the
// out-of-band XI_SYS_DROPPED marker. Consolidated here so the marker shape can't
// drift between the two. The bits that genuinely DIFFER stay at the call sites and
// ride in as params: `warn_count` is the counter the throttle reads (the per-LANE
// dropped total for a lane — read under the lane lock — vs the process-lifetime
// total for the capless one-shot path), `aid` is the arrival slot (claimed under
// the lane lock for drop_newest, so it stays at the call site to preserve ordering),
// and `policy`/`reason` carry each site's exact marker payload byte-for-byte. The
// counter bumps (g_eng.dropped_lifetime, and lane->dropped for the lane path) also
// stay at the call sites — the two paths bump different counter SETS and the lane
// counter must be touched under the lane lock.
static void account_dropped_frame_(xi::TriggerEvent& ev, uint64_t warn_count,
                                   int64_t aid, const char* policy, const char* reason) {
    release_trigger_event_(ev);            // the dropped event's image + doc refs
    warn_frame_drop_(warn_count, ev.group, policy);
    if (auto* srv = g_eng.srv_for_bp.load(std::memory_order_acquire))
        emit_run_result(*srv, XI_SYS_DROPPED, reason, aid, -1, ev.leader_source, ev.group,
                        trigger_id_hex(ev.id), "dropped", "queue_full");
}

// Per-lane enqueue with that lane's queue_depth/overflow policy.
static bool enqueue_to_lane_(xi::TriggerEvent ev) {
    // F7: releases ev on EVERY exit unless dismiss()'d (i.e. handed to a lane queue).
    TriggerEventReleaser guard(ev);
    if (!g_eng.continuous.load()) return false;
    std::shared_ptr<GroupLane> lane = lane_for_(ev.group);
    if (!lane) {
        // F2 START/RESUME WINDOW: cmd_start_ and DispatchPoolGuard::resume flip
        // g_eng.continuous=true BEFORE spawn_group_pool_ populates g_eng.lanes. A
        // concurrent source emit landing in that window routes here with the lane
        // set still empty (lane_for_ → nullptr). Historically this was a BARE
        // `return false`: the guard (TriggerEventReleaser, above) freed the refs so
        // nothing LEAKED, but the drop was otherwise INVISIBLE — no XI_SYS_DROPPED
        // marker and no counter bump, unlike EVERY other drop path. Route it through
        // the shared drop-accounting helper so a start/resume-window drop is
        // observable + counted. (This branch is unreachable once lanes are populated,
        // so the steady-state path stays byte-identical.)
        ++g_eng.dropped_lifetime;   // P1-8: lifetime total (no per-lane lane to bump here)
        // aid = -1: no arrival slot is claimed on this path (there is no lane to push
        // into, so no ++g_eng.run_id). account_dropped_frame_ releases ev internally;
        // release_trigger_event_ is idempotent (nulls ev.pack), so the guard's
        // destructor release that follows is a harmless no-op — no double-free.
        account_dropped_frame_(ev, g_eng.dropped_lifetime.load(), -1, "no_lane",
                               "dropped: no lane (start/resume window)");
        return false;
    }
    // depth 0 is now permitted (RENDEZVOUS lane — see the depth==0 branch below);
    // only genuinely-negative values clamp to the depth=0 floor. The parser
    // guarantees a depth==0 lane also has overflow=="block" (it warns + clamps to 1
    // otherwise), so the depth==0 branch's "never reach the front()-on-empty drop
    // path" invariant holds by construction.
    int depth = lane->cfg.queue_depth < 0 ? 0 : lane->cfg.queue_depth;
    const std::string& ov = lane->cfg.overflow;
    std::unique_lock<std::mutex> lk(lane->mu);
    // Re-check after taking the lane lock: a concurrent stop may have flipped
    // g_eng.continuous + drained; don't push a now-orphaned event (would leak).
    if (!g_eng.continuous.load()) return false;
    // ---- queue_depth:0 RENDEZVOUS (synchronous handoff; opt-in) ----------------
    // DANGER — like overflow:block, rendezvous is ONLY safe for a back-pressure-
    // TOLERANT source on a DEDICATED thread it can freely stall. NEVER a lane a
    // WORKER of this same lane can self-feed (a worker parked here can't drain its
    // own lane → self-deadlock until stop) and NEVER a real-time CAMERA-CALLBACK
    // thread (parking it stalls acquisition). Config-side responsibility — the code
    // can't tell a self-feeding thread from a dedicated one. A stop always breaks
    // the park (predicate keys off g_eng.continuous) so it degrades to a drop rather
    // than hanging teardown.
    if (depth == 0) {
        // Strict 1-wide handoff. Only ONE event can be in q at a time (each producer
        // waits for empty before depositing), so taken_count can't mis-attribute
        // another producer's take. Parser guarantees ov=="block" here, so this
        // returns before any drop_oldest front()-on-empty path could run.
        // 1) Wait for the handoff slot to be free (or stop).
        lane->cv_not_full.wait(lk, [&] {
            return lane->q.empty() || !g_eng.continuous.load();
        });
        if (!g_eng.continuous.load()) {                      // stop → drop (guard releases ev)
            // RB3 (doc 25): a producer parked here at stop drops an in-hand frame.
            // Count it (frames-in vs verdicts+drops-out stays balanced) but do NOT
            // emit an XI_SYS_DROPPED wire marker: this is teardown (unlike F2's
            // start-window drop), the monitor is tearing down too, and emitting under
            // the lane lock would need an unlock dance. Counter-only, by design.
            ++g_eng.dropped_lifetime;
            return false;
        }
        // 2) Snapshot the take-generation UNDER lk, then deposit.
        uint64_t t0 = lane->taken_count;
        ev.arrival_id = ++g_eng.run_id;
        lane->q.push_back(std::move(ev)); guard.dismiss();   // ownership → queue
        // Raise the peak (rendezvous tops out at 1 — the whole point: the core lane
        // never buffers more than the single in-flight handoff).
        { uint64_t ns = lane->q.size(), prev = lane->high_watermark.load(std::memory_order_relaxed);
          while (ns > prev && !lane->high_watermark.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {}
          uint64_t gprev = g_eng.high_watermark_lifetime.load(std::memory_order_relaxed);
          while (ns > gprev && !g_eng.high_watermark_lifetime.compare_exchange_weak(gprev, ns, std::memory_order_relaxed)) {} }
        lane->cv.notify_one();                               // wake a worker to take it
        // 3) Block until OUR event is TAKEN (taken_count advanced) or stop. On stop
        //    the event is already in q; stop_group_pool_ drains it (no leak, no hang).
        lane->cv_not_full.wait(lk, [&] {
            return lane->taken_count != t0 || !g_eng.continuous.load();
        });
        return true;   // handed off (or stop-woken after deposit — teardown drains q)
    }
    // ---- overflow:"block" back-pressure (opt-in; default stays drop_oldest) -----
    // DANGER — block is ONLY safe for a back-pressure-TOLERANT source that emits on
    // a DEDICATED thread it can freely stall (a file/disk batch reader, a dedicated
    // timer/emit thread). NEVER point a block lane at:
    //   * a lane a WORKER of this same lane can feed (a worker parked here can't
    //     drain its own lane → self-deadlock until stop), or
    //   * a real-time CAMERA-CALLBACK thread (parking it stalls acquisition / makes
    //     the driver drop at the hardware instead — worse than a clean drop here).
    // Enforcing that is a CONFIG-side responsibility (see the parser/model doc); the
    // code cannot tell a self-feeding thread from a dedicated one. A stop always
    // breaks any such park (predicate keys off g_eng.continuous) so it degrades to a
    // drop rather than hanging teardown — but a mis-pointed block lane will stall its
    // producer for the whole run.
    if ((int)lane->q.size() >= depth && ov == "block") {
        // Park until a slot frees OR the lane stops. INTERRUPTIBLE by design — a stop
        // (continuous flips false) or teardown wakes us via cv_not_full and we DEGRADE
        // TO DROP rather than hang. cv.wait releases lane->mu while parked so the
        // worker can drain and free a slot; it re-acquires lk before returning.
        lane->cv_not_full.wait(lk, [&] {
            return (int)lane->q.size() < depth || !g_eng.continuous.load();
        });
        if (!g_eng.continuous.load()) {   // stop-wake → drop (guard releases ev)
            ++g_eng.dropped_lifetime;     // RB3 (doc 25): count the teardown drop (no wire marker — see depth-0 note)
            return false;
        }
        // Fall through: a slot is now free AND we still hold lk, so no other producer
        // can steal it before our push below (cv.wait returned with lane->mu HELD).
    }
    if ((int)lane->q.size() < depth) {
        ev.arrival_id = ++g_eng.run_id;   // arrival/run id in push (== FIFO) order
        lane->q.push_back(std::move(ev)); guard.dismiss();   // ownership → queue
        uint64_t ns = lane->q.size(), prev = lane->high_watermark.load(std::memory_order_relaxed);
        while (ns > prev && !lane->high_watermark.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {}
        // P1-8: also raise the process-lifetime peak (survives cmd:start).
        uint64_t gprev = g_eng.high_watermark_lifetime.load(std::memory_order_relaxed);
        while (ns > gprev && !g_eng.high_watermark_lifetime.compare_exchange_weak(gprev, ns, std::memory_order_relaxed)) {}
        lane->cv.notify_one(); return true;
    }
    if (ov == "drop_newest") {
        ++lane->dropped; ++g_eng.dropped_lifetime;   // P1-8: lifetime total survives cmd:start
        int64_t aid = ++g_eng.run_id;   // arrival slot of the dropped (new) frame
        uint64_t wc = lane->dropped.load();   // throttle reads the per-lane total (under lock)
        lk.unlock();
        guard.dismiss();   // account_dropped_frame_ releases the dropped (new) ev
        // Out-of-band NA marker (not gated — gating would stall the source); the
        // run_id lets a consumer order it against this lane's run results.
        account_dropped_frame_(ev, wc, aid, "drop_newest", "dropped: queue full (drop_newest)");
        return false;
    }
    auto& front = lane->q.front();   // drop_oldest (the default + fallback)
    int64_t dropped_aid = front.arrival_id;   // the dropped (oldest) frame's slot
    std::string ds = front.leader_source, dg = front.group;   // the dropped (oldest) event
    std::string dt = trigger_id_hex(front.id);                // its trigger id (additive)
    release_trigger_event_(front);   // the evicted front (a different event, in-queue)
    lane->q.pop_front(); ev.arrival_id = ++g_eng.run_id; lane->q.push_back(std::move(ev)); guard.dismiss();   // ev → queue
    lane->cv.notify_one(); ++lane->dropped; ++g_eng.dropped_lifetime;   // P1-8
    warn_frame_drop_(lane->dropped.load(), dg, "drop_oldest");
    lk.unlock();
    if (auto* srv = g_eng.srv_for_bp.load(std::memory_order_acquire))
        emit_run_result(*srv, XI_SYS_DROPPED, "dropped: queue full (drop_oldest)", dropped_aid, -1, ds, dg,
                        dt, "dropped", "queue_full");
    return true;
}

void spawn_group_pool_(xi::ws::Server* srv_ptr, int interval_ms) {
    {
        std::lock_guard<std::mutex> lk(g_eng.lanes_mu);
        // F8: callers must stop_dispatch_pool_ before (re)spawning — a non-empty
        // g_eng.lanes here means a prior pool's workers are still running and we'd
        // silently double-spawn (two worker sets draining one source). All sites
        // pair stop+spawn today; assert the invariant so a future site can't
        // regress it silently. Release builds also leave a stderr breadcrumb since
        // assert() is compiled out there.
        assert(g_eng.lanes.empty() && "spawn_group_pool_ called without a preceding stop_dispatch_pool_");
        if (!g_eng.lanes.empty())
            std::fprintf(stderr, "[xinsp2] BUG: spawn_group_pool_ with %zu live lane(s) — "
                         "missing stop_dispatch_pool_; clearing (workers may double-run)\n",
                         g_eng.lanes.size());
        g_eng.lanes.clear();
        // F4: capture default_group with this lane set so lane_for_ routes against
        // a name that exists in g_eng.lanes (the synthesized default lane below is "").
        g_eng.default_group_snapshot = g_eng.plugin_mgr.project().default_group;
        for (auto& gc : g_eng.plugin_mgr.project().groups) {
            auto lane = std::make_shared<GroupLane>(); lane->cfg = gc; g_eng.lanes.push_back(std::move(lane));
        }
        if (g_eng.lanes.empty()) {
            // No explicit groups: synthesize ONE default lane from the project's
            // parallelism settings, so every project runs on the unified lane
            // path (the legacy single pool is gone). name "" matches an untagged
            // event's empty group via lane_for_(); the timer tick also targets
            // default_group (== "" here) -> this lane.
            const auto& p = g_eng.plugin_mgr.project();
            xi::ProjectInfo::DispatchGroup def;
            def.name         = "";
            def.max_parallel = p.dispatch_threads < 1 ? 1 : p.dispatch_threads;
            def.queue_depth  = p.queue_depth;
            def.overflow     = p.overflow;
            def.result_order = p.result_order;
            auto lane = std::make_shared<GroupLane>(); lane->cfg = def;
            g_eng.lanes.push_back(std::move(lane));
        }
    }
    std::fprintf(stderr, "[xinsp2] continuous mode (grouped): %zu group(s), %dms timer\n",
                 g_eng.lanes.size(), interval_ms);
    { int total = 0; for (auto& lp : g_eng.lanes) total += (lp->cfg.max_parallel < 1 ? 1 : lp->cfg.max_parallel);
      warn_oversubscribe_(total); }
    for (auto& lp : g_eng.lanes) {
        std::shared_ptr<GroupLane> lane = lp;   // workers hold a ref → lane outlives them
        int n = lane->cfg.max_parallel < 1 ? 1 : lane->cfg.max_parallel;
        // RB2 (doc 25): a queue_depth:0 (rendezvous) lane is STRICT SERIAL by
        // definition — the producer releases at TAKE time, so >1 idle worker would
        // pop-and-run inspections CONCURRENTLY, breaking the advertised 1-in-flight
        // guarantee (and racing a non-reentrant script the user picked depth-0 to
        // serialize). Force a single worker so the guarantee is REAL. (This clamps the
        // WORKER COUNT at spawn, not the config value — max_parallel is left as-set;
        // multi-threaded admission is the plugin-semaphore path over a normal lane,
        // NOT a core depth-0 lane. See doc 24 §4 / qa_semaphore_queue.)
        if (lane->cfg.queue_depth == 0) n = 1;
        // Arrival-ordered emission only when asked AND there's real concurrency
        // (n==1 is already in order). Reset the per-lane cursors per (re)start.
        lane->ordered = (lane->cfg.result_order == "arrival") && n > 1;
        lane->seq_next.store(0);
        lane->next_allowed_us.store(0);
        { std::lock_guard<std::mutex> lk(lane->gate.mu); lane->gate.next = 0; }
        for (int i = 0; i < n; ++i) {
            lane->workers.emplace_back([srv_ptr, lane, wi = i] {
                reserve_fault_stack();
                xi::install_seh_translator();
                set_os_thread_priority_(lane->cfg.thread_priority);
                // CPU affinity (empty = unbound). One mask → all workers share it;
                // N masks → worker wi uses set[wi % N]. Each mask may be multi-core.
                const auto& aff = lane->cfg.cpu_affinity;
                if (!aff.empty())
                    set_os_thread_affinity_(aff.size() == 1 ? aff[0] : aff[(size_t)wi % aff.size()]);
                while (g_eng.continuous.load()) {
                    xi::TriggerEvent ev; bool have = false; int64_t rid = 0; int64_t eseq = -1;
                    {
                        std::unique_lock<std::mutex> lk(lane->mu);
                        lane->cv.wait(lk, [lane] { return !lane->q.empty() || !g_eng.continuous.load(); });
                        if (!g_eng.continuous.load()) break;
                        if (!lane->q.empty()) {
                            ev = std::move(lane->q.front()); lane->q.pop_front(); have = true;
                            // depth=0 rendezvous: advance the take-generation UNDER lk
                            // (paired with a depth=0 producer's under-lk snapshot) so a
                            // producer parked in the rendezvous learns ITS event was taken.
                            // The outside-lock cv_not_full.notify_all() below wakes it.
                            ++lane->taken_count;
                            // run_id was claimed at ENQUEUE (push == FIFO order) so kept
                            // and dropped frames share one arrival sequence; read it back
                            // (fallback if unset). The emit seq (gate ordering) is still
                            // claimed here (ordered mode) — drops leave no gate gap.
                            rid = ev.arrival_id ? ev.arrival_id : ++g_eng.run_id;
                            if (lane->ordered) eseq = lane->seq_next.fetch_add(1);
                        }
                    }
                    // Two cvs, split by role: WORKERS wait on lane->cv (notify_one —
                    // one freed slot wakes one worker), PRODUCERS (overflow:block AND
                    // depth-0 rendezvous) park on lane->cv_not_full. A pop frees a slot,
                    // so wake a worker (cv) and the producers (cv_not_full).
                    // RB1 (doc 25): cv_not_full must be notify_ALL, not notify_one — the
                    // depth-0 producer does TWO distinct waits on this cv (slot-free, then
                    // its-event-taken). With ≥2 producers on one depth-0 lane a single
                    // notify could wake a slot-free waiter instead of the taken-waiter
                    // whose event this pop just consumed, stranding that producer until
                    // stop. notify_all + the under-lock predicate re-check (harmless
                    // thundering-herd at lane-drain rate) closes the lost-wakeup.
                    lane->cv.notify_one();
                    lane->cv_not_full.notify_all();
                    if (!have) continue;
                    // Rate limit: CAS-claim a dispatch slot ≥ min_interval after the
                    // lane's previous one, then sleep to it. Surplus events meanwhile
                    // pile in the queue and coalesce via drop_oldest (latest wins).
                    if (lane->cfg.min_interval_ms > 0) {
                        // steady clock: a wall-clock (NTP/DST) jump must not stall
                        // the lane for the duration of the jump. next_allowed_us
                        // holds steady-us (reset to 0 at lane start).
                        int64_t iv = (int64_t)lane->cfg.min_interval_ms * 1000;
                        int64_t now = xi::steady_now_us(), prev = lane->next_allowed_us.load(std::memory_order_relaxed), slot;
                        do { slot = prev > now ? prev : now; }
                        while (!lane->next_allowed_us.compare_exchange_weak(prev, slot + iv, std::memory_order_acq_rel));
                        for (int64_t w = slot - xi::steady_now_us(); w > 0 && g_eng.continuous.load(); w = slot - xi::steady_now_us())
                            std::this_thread::sleep_for(std::chrono::microseconds(w > 20000 ? 20000 : w));
                    }
                    ev.dequeued_at_us = xi::now_us();
                    lane->running.fetch_add(1);
                    int frame_seq = (int)rid;
                    // THE CUT: "is this a real trigger?" is now keyed on pack payload
                    // presence (the Record-era ev.images emptiness check is gone). Use the
                    // trigger_bus owner's helper TriggerEvent::is_real() (== pack != XI_PACK_NULL).
                    if (ev.is_real() || ev.id.hi || ev.id.lo) {
                        CurrentTriggerScope trig(ev);   // clears g_current_trigger + releases ev on scope exit
                        run_one_inspection(*srv_ptr, frame_seq, rid, "", eseq, &lane->gate);
                    } else {
                        run_one_inspection(*srv_ptr, frame_seq, rid, "", eseq, &lane->gate);
                    }
                    lane->running.fetch_sub(1);
                }
            });
        }
    }
    // Timer ticks feed the default group's lane. Always spawned; reads the LIVE
    // g_eng.timer_interval_ms each loop so the rate is retunable mid-run and 0 =
    // trigger-only (the default group isn't loaded with synthetic ticks).
    (void)interval_ms;
    g_eng.timer_thread = std::thread([] {
        const std::string dg = g_eng.plugin_mgr.project().default_group;
        while (g_eng.continuous.load()) {
            int iv = g_eng.timer_interval_ms.load();
            if (iv <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(iv));
            if (!g_eng.continuous.load()) break;
            if (g_eng.timer_interval_ms.load() <= 0) continue;
            xi::TriggerEvent ev; ev.group = dg;
            (void)enqueue_to_lane_(std::move(ev));
        }
    });
}

void stop_group_pool_() {
    // Snapshot the lanes (under the lock) so producers can keep routing into the
    // shared_ptrs while we tear down; new enqueues already bail on !g_eng.continuous.
    std::vector<std::shared_ptr<GroupLane>> lanes;
    { std::lock_guard<std::mutex> lk(g_eng.lanes_mu); lanes = g_eng.lanes; }
    // Wake workers (cv) AND any overflow:block producer parked on cv_not_full — the
    // block-wait predicate keys off g_eng.continuous (already false here), so a parked
    // producer returns-and-drops instead of hanging teardown (its join below / the
    // source-instance destroy later would otherwise deadlock on a stuck emit thread).
    for (auto& lp : lanes) { std::lock_guard<std::mutex> lk(lp->mu); lp->cv.notify_all(); lp->cv_not_full.notify_all(); }
    for (auto& lp : lanes) for (auto& t : lp->workers) if (t.joinable()) t.join();
    // Workers are gone + g_eng.continuous is false → drain leftover queued events and
    // release their image handles before the lanes are dropped (release-before-
    // FreeLibrary; #3 leak fix).
    for (auto& lp : lanes) {
        std::lock_guard<std::mutex> lk(lp->mu);
        for (auto& ev : lp->q) release_trigger_event_(ev);
        lp->q.clear();
    }
    { std::lock_guard<std::mutex> lk(g_eng.lanes_mu); g_eng.lanes.clear(); }
}

// Stop the pool + timer. Safe to call if nothing was spawned.
void stop_dispatch_pool_() {
    g_eng.continuous = false;
    // Wake the lane workers (so they observe g_eng.continuous=false and exit) BEFORE
    // joining, or the join deadlocks. Also wake anyone parked in a per-lane EmitTurn
    // (ordered mode).
    {
        std::vector<std::shared_ptr<GroupLane>> lanes;
        { std::lock_guard<std::mutex> lk(g_eng.lanes_mu); lanes = g_eng.lanes; }
        for (auto& lp : lanes) {
            // cv → workers; cv_not_full → any parked overflow:block producer (so it
            // observes continuous=false and returns-and-drops, not hang); gate.cv →
            // anyone parked in a per-lane EmitTurn (ordered mode).
            { std::lock_guard<std::mutex> lk(lp->mu); lp->cv.notify_all(); lp->cv_not_full.notify_all(); }
            { std::lock_guard<std::mutex> lk(lp->gate.mu); lp->gate.cv.notify_all(); }
        }
    }
    if (g_eng.timer_thread.joinable()) g_eng.timer_thread.join();
    stop_group_pool_();
}

// Trigger-driven dispatch WITHOUT continuous mode: a source emitting a trigger
// (e.g. a webui "issue"/"replay" click) runs exactly ONE inspect on it. The emit
// usually arrives on the WS thread (inside a plugin's exchange), so we run the
// inspect on a detached thread, not inline. Serialized by g_eng.run_mu; the
// thread_local g_current_trigger makes this thread's inspect see this event.
static void dispatch_one_shot_(xi::ws::Server* srv, xi::TriggerEvent ev) {
    auto evp = std::make_shared<xi::TriggerEvent>(std::move(ev));
    // The lambda runs on a source plugin's emit thread, which outlives the
    // main-local srv — g_eng.inflight owns the bump/bail/drain so teardown waits it
    // out. On a bail (tearing down or thread-spawn failure) OR a cap drop we
    // release the event's image/meta handles ourselves.
    bool dropped_over_cap = false;
    bool launched = g_eng.inflight.launch([srv, evp]() {
        reserve_fault_stack();
        xi::install_seh_translator();
        std::lock_guard<std::mutex> lk(g_eng.run_mu);
        CurrentTriggerScope trig(*evp);   // clears g_current_trigger + releases evp on scope exit
        run_one_inspection(*srv, /*frame_hint=*/0, /*run_id=*/0, "", /*emit_seq=*/-1);
    }, &dropped_over_cap);
    if (!launched) {
        // B1: on ANY non-launch we must release the dropped event's image/meta refs
        // ourselves (the CurrentTriggerScope that normally does it never ran) — the
        // same release-on-drop discipline the continuous lane path uses, so a
        // dropped one-shot leaks no ImagePool/DocRegistry handles.
        if (dropped_over_cap) {
            // At the in-flight cap this is an OVERFLOW drop-newest: mirror the
            // continuous GroupLane drop_newest path via the shared helper — bump the
            // lifetime dropped counter and emit an out-of-band XI_SYS_DROPPED marker
            // (release happens inside the helper) so the drop is observable/counted.
            int64_t aid = ++g_eng.run_id;              // arrival slot of the dropped (new) frame
            ++g_eng.dropped_lifetime;                  // P1-8: lifetime total survives cmd:start
            account_dropped_frame_(*evp, g_eng.dropped_lifetime.load(), aid, "max_inflight",
                                   "dropped: max in-flight one-shots reached");
        } else {
            // A bare shutdown/pause bail is not a drop (no marker) — just release.
            release_trigger_event_(*evp);
        }
    }
}

// SINGLE SOURCE OF TRUTH for process-exit teardown. Called from BOTH the
// cmd:shutdown handler and the main() epilogue (which covers exits that didn't go
// through cmd:shutdown). Previously this sequence was hand-copied in both places
// and had drifted — the epilogue copy was missing reset() and never stopped the
// dispatch pool — which is exactly the class of teardown bug the audit rounds
// kept hitting. Run it once, in this fixed order:
//   1. stop every emit SOURCE first (dispatch pool, replay, in-flight detached
//      run) so no inspect emits after this point,
//   2. drop the bus sink/observer + release bus-held image handles,
//   3. unload the script under its lock (its module deleter runs while the
//      ImagePool is still alive),
//   4. drop the srv pointer LAST (after every emitter is quiesced).
// Idempotent: safe to call twice (the shutdown handler runs it, then the epilogue
// runs it again as the loop unwinds).
void controlled_shutdown_teardown_() {
    // Refuse NEW detached runs first, then drop the bus sink so no source emit
    // launches another one-shot. (g_eng.inflight.launch() checks this — a launch
    // racing teardown either bails or is waited out by drain() below.)
    g_eng.inflight.begin_shutdown();
    xi::TriggerBus::instance().clear_sink();
    if (g_eng.continuous.load()) stop_dispatch_pool_();   // joins workers + timer + drains lanes
    // Drain in-flight detached cmd:run / one-shot threads. A bare g_eng.run_mu acquire
    // only waits for a thread that already HOLDS the lock; one detached-but-not-yet-
    // locked would slip past and then touch the about-to-be-destroyed srv. drain()
    // waits on the in-flight count (capped) instead.
    //
    // T1: if drain() TIMES OUT (a wedged inspect — infinite loop / blocking plugin —
    // still in flight after the cap), we must NOT proceed into teardown: close_project
    // below FreeLibrary's the plugin DLLs and g_eng.srv_for_bp is nulled while that thread
    // is still inside run_one_inspection(*srv)/process_fn_ → UAF / access-violation.
    // With the watchdog DISABLED (default g_eng.watchdog_ms{0}) nothing else would have
    // killed the process, so this path is reachable. Do exactly what the watchdog's
    // HARD-trip does: log, flush, and std::_Exit(WATCHDOG_EXIT_CODE) — a crash-safe
    // hard exit (skips static dtors / atexit a wedged worker could deadlock) that the
    // FE supervisor respawns. A clean teardown here is unsafe precisely BECAUSE a
    // thread is wedged; the hard exit is the correct trade.
    if (!g_eng.inflight.drain()) {
        int stuck = g_eng.inflight.inflight();
        std::fprintf(stderr,
            "[xinsp2] shutdown drain TIMED OUT with %d wedged in-flight inspect(s) — "
            "hard-exiting instead of tearing down (would UAF: FreeLibrary + srv destroy "
            "under a live detached run); FE supervisor respawns (rc=0x%04X)\n",
            stuck, WATCHDOG_EXIT_CODE);
        if (auto* s = g_eng.srv_for_bp.load(std::memory_order_acquire))
            emit_error_log(*s,
                "shutdown drain timed out with " + std::to_string(stuck) +
                " wedged in-flight inspect(s); backend hard-exiting for respawn");
        std::fflush(stderr);
        std::fflush(stdout);
        // H7: let an in-flight minidump on the wedged worker finish before we
        // hard-exit (bounded — the drain already timed out, don't stall forever).
        xi::crash::await_dump(10000);
        std::_Exit(WATCHDOG_EXIT_CODE);
    }
    { std::lock_guard<std::mutex> rl(g_eng.run_mu); }     // belt-and-suspenders
    xi::TriggerBus::instance().reset();               // prune the per-source emit-time map (source names go out of scope here)
    { std::lock_guard<std::mutex> lk(g_eng.script_mu); xi::script::unload_script(g_eng.script); }
    // Close the open project (if any) NOW — while the ImagePool singleton is still
    // alive — so plugin instances are destroyed in the correct order (instances first,
    // THEN FreeLibrary) and their image-handle sweep runs against a live pool. The
    // normal exit paths (cmd:shutdown, g_eng.should_exit epilogue) otherwise never called
    // close_project, leaving ~PluginManager to do it at static destruction — after
    // FreeLibrary (destroy_fn into unmapped code) and after ImagePool was torn down
    // (release_all_for on a destroyed singleton). Idempotent: no-op if already closed.
    g_eng.plugin_mgr.close_project();
    g_eng.srv_for_bp = nullptr;                            // last: every emitter is quiesced now
    g_eng.teardown_done.store(true);                       // T2: unblock a waiting console handler
}

// T2 — orderly shutdown on an abrupt console exit (window close, Ctrl+C/Break,
// logoff, system shutdown), which otherwise bypasses controlled_shutdown_teardown_
// entirely (the OS default handler ExitProcess'es: no plugin destructors, so a
// comm/PLC plugin's "go-safe on close" never fires, and the still-armed crash
// filter can turn the kill into a spurious minidump the FE reads as a crash).
// The handler just flips g_eng.should_exit — the main serve loop polls it every 100ms
// and runs the SAME controlled_shutdown_teardown_ path. For the close-class events
// the OS force-terminates shortly after the handler returns, so we BLOCK (bounded)
// until teardown signals done, keeping the process alive long enough to tear down
// cleanly in the common (no-wedge) case. Ctrl+C/Break have no hard deadline.
// Registered only in main() (the backend exe) — never affects embedded/SDK use.
#ifdef _WIN32
BOOL WINAPI console_ctrl_handler_(DWORD type) {   // decl in header (used by main)
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: {
            std::fprintf(stderr, "[xinsp2] console control event %lu — clean shutdown\n",
                         (unsigned long)type);
            std::fflush(stderr);
            g_eng.should_exit.store(true);
            const bool close_class = (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT);
            if (close_class) {
                // ~4.5s budget (< the OS's default ~5s CTRL_CLOSE window) for main()
                // to run teardown. If teardown hard-exits on a wedged drain (T1), the
                // process is already gone; this loop just falls through on timeout.
                for (int i = 0; i < 450 && !g_eng.teardown_done.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return TRUE;   // handled — suppress the default terminate
        }
        default:
            return FALSE;
    }
}
#endif

// Install the bus sink so triggers always dispatch: in continuous mode they feed
// the worker-pool queue; otherwise each trigger runs a single-shot inspect. This
// is installed on every compile_and_load so "issue"/"replay" works WITHOUT needing
// cmd:start — the trigger-driven model (continuous is just an optional free-running
// timer on top).
void install_trigger_sink_(xi::ws::Server* srv) {
    // B1: apply the project's one-shot in-flight ceiling. Installed on every
    // compile_and_load, so a project's parallelism.max_inflight takes effect
    // WITHOUT needing cmd:start (one-shot dispatch works pre-start). <=0 → default.
    g_eng.inflight.set_cap(g_eng.plugin_mgr.project().max_inflight);
    xi::TriggerBus::instance().set_sink([srv](xi::TriggerEvent ev) {
        if (g_eng.continuous.load()) {
            // Route by the emitting source instance's "group" (default_group if
            // the source is untagged/unknown, or the synthetic timer tick). A
            // project with no explicit groups resolves to the synthesized default
            // lane (group "") — see spawn_group_pool_. instance_group() does the
            // lookup UNDER PluginManager's lock — this sink runs on a source's emit
            // thread, concurrent with create/remove/rename_instance.
            ev.group = g_eng.plugin_mgr.instance_group(ev.leader_source);
            (void)enqueue_to_lane_(std::move(ev));
        } else {
            dispatch_one_shot_(srv, std::move(ev));
        }
    });

}

// Quiesce the dispatcher + timer before any handler
// that's about to touch the script DLL or project plugin DLLs (audit
// P0-AB-1..5). Returns the prior continuous state so the caller can
// re-arm if it wants. Mirrors the inline block compile_and_load has
// used since FL r5.
//
// IMPORTANT: this only quiesces the bus-driven dispatcher pool. Any
// detached cmd:run threads are not joined here — those snapshot the
// script under g_eng.script_mu and the destructive caller still holds
// g_eng.script_mu (or equivalent) while the inspect runs to completion.
// For project plugin DLLs (touched by close/open/recompile/export),
// no equivalent detached path exists; the dispatch pool is the only
// in-flight surface.
// RAII: stop continuous dispatch for a lifecycle op (DLL unload/reload/commit) and
// RESUME it when the guard goes out of scope. Resume-on-destruction is the default
// so "quiesce must be symmetric with resume" is guaranteed by the type, not by each
// handler remembering to respawn — five handlers (recompile/rebuild/commit/discard/
// export) used to quiesce and never resume, silently stopping the live stream on a
// hot-recompile. An op that legitimately should NOT resume (the stream ends or is
// replaced: unload_script / close_project / open_project) calls dismiss().
// MOVE-ONLY: quiesce_dispatch_for_lifecycle_op_ returns one by value; a moved-from
// guard won't resume. CALLERS MUST HOLD IT (`auto g = quiesce_...`) for the op's
// duration — a discarded temporary would resume immediately, before the op runs.
// DispatchPoolGuard struct (incl. its inline resume()/dismiss()) moved to
// service_internal.hpp so lifecycle-op cmd handlers in other TUs can hold it.
DispatchPoolGuard quiesce_dispatch_for_lifecycle_op_(const char* op_name,
                                                            xi::ws::Server* srv) {
    DispatchPoolGuard g;
    g.srv = srv;
    // Stop NEW detached one-shot / cmd:run launches and drop the bus sink BEFORE the
    // op FreeLibrary's any plugin DLL. A one-shot dispatch runs on a SOURCE plugin's
    // own emit thread (bus sink -> dispatch_one_shot_ -> g_eng.inflight.launch), NOT this
    // handler thread — so without this a source emitting mid-op could launch an
    // inspect that calls into a DLL being unloaded (use-after-unload). clear_sink stops
    // future fires; pause()+drain() is the Dekker handshake that also catches an emit
    // already past the sink read but not yet counted. The guard reverses both (resume
    // re-installs the sink + unpauses; dismiss unpauses without re-installing).
    g_eng.inflight.pause();
    g.paused_launches_ = true;
    g.restore_sink_    = xi::TriggerBus::instance().has_sink();   // only restore if one existed
    xi::TriggerBus::instance().clear_sink();
    if (g_eng.continuous.load()) {
        g.was_continuous = true;
        g.prior_fps = g_eng.continuous_fps.load();
        stop_dispatch_pool_();
        g.quiesced = true;
        std::fprintf(stderr,
            "[xinsp2] stopped continuous mode for %s (resumes when the op completes)\n",
            op_name);
    }
    // (Lane queues are drained + their image handles released inside
    // stop_dispatch_pool_ -> stop_group_pool_ above, before the DLLs unload.)
    // Wait out any in-flight detached cmd:run / one-shot inspect already running:
    // they hold g_eng.run_mu for the whole inspect and call into the plugin/script DLLs
    // this op is about to FreeLibrary. drain() waits on the in-flight count (paused
    // above so none can start meanwhile); the g_eng.run_mu acquire is belt-and-suspenders.
    g_eng.inflight.drain();
    { std::lock_guard<std::mutex> lk(g_eng.run_mu); }
    return g;
}

// ---------------------------------------------------------------------------
// Host-tracked instance state (orchestrator get_state — task #67).
//
// A deliberately tiny state machine the HOST keeps per instance, driven by the
// host-visible orchestrator verbs. Fine staging/ready sub-state stays plugin-
// side (exchange get_status); the host records only these three coarse states +
// the last error. In the exchange-convention prototype the host can't see
// prepare/commit (they ride opaque exchange commands), so only create_instance,
// set_instance_def and commit_group move the state — once prepare/commit are
// first-class ABI (task #69) the host will observe them directly and refine this.
// ---------------------------------------------------------------------------
// The state map itself now lives in the PluginManager, OWNED alongside the
// instance set under its one mutex, so create/remove/rename migrate it atomically
// and it can never drift out of sync (the bug class the earlier rounds kept
// hitting). These free functions are thin pass-throughs kept only so the call
// sites below read the same as before. drop_inst_state / rename_inst_state are
// GONE on purpose: remove_instance / rename_instance migrate the state inline, so
// the WS handlers must NOT do it separately.
using xi::InstState;

const char* inst_state_str(InstState s) {
    switch (s) { case InstState::Active: return "active";
                 case InstState::Faulted: return "faulted";
                 default: return "created"; }
}

void set_inst_state(const std::string& name, InstState s,
                           const std::string& err) {
    g_eng.plugin_mgr.set_instance_state(name, s, err);
    // Health contract: re-activating an instance clears any runtime-fault overlay
    // (an operator re-committing a crash-quarantined instance recovers it). The
    // base active/faulted health is DERIVED from InstState at get_health time, so
    // there is nothing else to mirror here. clear is a no-op if it wasn't degraded.
    if (s == InstState::Active) {
        xi::health().clear_instance_degraded(name);
        // item 14: re-committing an instance's config (set_instance_def /
        // commit_group set it Active) is the EXISTING operator re-enable surface for
        // an on_fault=refuse quarantine — lift the fail-fast gate + reset the reinit
        // escalation so the pulled instance is put back in service. No new command.
        if (auto inst = xi::InstanceRegistry::instance().find(name)) {
            if (auto* a = dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get())) {
                a->set_quarantined(false);
                a->reset_reinit_fails();
            }
        }
    }
}
void clear_inst_state() {
    g_eng.plugin_mgr.clear_instance_states();
}
