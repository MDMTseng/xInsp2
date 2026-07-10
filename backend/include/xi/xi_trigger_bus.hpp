#pragma once
//
// xi_trigger_bus.hpp — the host-side dispatch funnel for emit_pack.
//
// A source plugin pushes a sealed pack (frames + metadata, keyed buffers) via
// host->emit_pack. The bus builds ONE TriggerEvent per emit and hands it to the
// subscribed worker (the script's inspect loop). There is no correlation: a
// source that wants several frames inspected together (e.g. a gathering stereo
// source) puts them in the SAME pack, so the bus is a pure funnel — emit in,
// dispatch out. (Multi-camera sync, recording/replay, and trigger policies were
// removed in the ABI-v6 dispatch cleanup: sync is a gathering plugin, replay is
// a buffer-replay plugin.)
//
// The pack handle inside a TriggerEvent is owned by the bus; the worker that
// consumes the event releases it (via TriggerBus::release_pack_) after dispatch.
//

#include "xi_abi.h"
#include "xi_clock.hpp"
#include "xi_image_pool.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xi {

struct TriggerEvent {
    xi_trigger_id  id{0, 0};
    int64_t        timestamp_us = 0;          // emit (capture) timestamp
    // Stamped by the dispatcher worker the moment this event is popped off the
    // dispatch queue. Same clock as timestamp_us (system_clock µs — see
    // xi::now_us()), so scripts can compute queue_wait_us = dequeued_at_us -
    // timestamp_us and inspect_us = xi::now_us() - dequeued_at_us. 0 until the
    // worker stamps it.
    int64_t        dequeued_at_us = 0;
    // Arrival/run id, assigned by the backend dispatcher when the frame is
    // committed to a dispatch queue (in push == FIFO dequeue order). The worker
    // uses it as the run_id; a dropped frame's marker carries it too. 0 until
    // the dispatcher assigns it.
    int64_t        arrival_id = 0;
    // The emitting instance's name — current_trigger().primary_source().
    std::string    leader_source;
    // Dispatch group (priority/concurrency lane). Stamped by the dispatcher sink
    // from the emitting instance's "group" (default_group if untagged).
    std::string    group;
    // The event payload: a sealed, immutable, refcounted host pack handle (the v3
    // keyed-buffer Pack plane). The pack carries the frames AND the metadata doc;
    // both are read through the Pack accessors, not off the event. The
    // ordering/EmitGate machinery keys on id + arrival_id, payload-agnostically.
    // The bus took ONE ref on emit_pack; release_trigger_event_() drops it at the
    // drop/consume site via TriggerBus::release_pack_(). XI_PACK_NULL marks a
    // non-payload event (e.g. a drop marker) — see is_real().
    xi_pack_handle pack = XI_PACK_NULL;

    // Pack-aware "is this a real trigger" predicate: true iff the event carries a
    // payload pack. Keyed on pack presence (the Record-era image/doc members that
    // an emptiness check would have used were deleted by THE CUT).
    bool is_real() const { return pack != XI_PACK_NULL; }

    // Move semantics are USER-DECLARED so a moved-from husk truly owns nothing.
    // `pack` is a scalar handle with no destructor, so the implicit move merely
    // COPIED it and left the source still set: any path that moves an event into
    // a queue and later releases the husk (a missed/late TriggerEventReleaser
    // dismiss(), or a throw inserted between the push and the dismiss) would
    // double-release a handle the queue now owns — a UAF waiting for a refactor.
    // Nulling the source's pack here makes the F7 guard's documented invariant
    // ("a move leaves ev empty, so even a missed dismiss() releases nothing —
    // never a double-free", service_sinks.cpp) actually hold. Copy stays
    // defaulted: a copy intentionally ALIASES the handle without taking a ref —
    // the manual release discipline (release_trigger_event_) is unchanged.
    // Note: like the implicit assign it replaces, move-assign does NOT release
    // this->pack before overwriting (this header has no releaser); callers own
    // releasing a live event before assigning over it.
    TriggerEvent() = default;
    TriggerEvent(const TriggerEvent&)            = default;
    TriggerEvent& operator=(const TriggerEvent&) = default;
    TriggerEvent(TriggerEvent&& o) noexcept
        : id(o.id),
          timestamp_us(o.timestamp_us),
          dequeued_at_us(o.dequeued_at_us),
          arrival_id(o.arrival_id),
          leader_source(std::move(o.leader_source)),
          group(std::move(o.group)),
          pack(o.pack) {
        o.pack = XI_PACK_NULL;
    }
    TriggerEvent& operator=(TriggerEvent&& o) noexcept {
        if (this != &o) {
            id             = o.id;
            timestamp_us   = o.timestamp_us;
            dequeued_at_us = o.dequeued_at_us;
            arrival_id     = o.arrival_id;
            leader_source  = std::move(o.leader_source);
            group          = std::move(o.group);
            pack           = o.pack;
            o.pack         = XI_PACK_NULL;
        }
        return *this;
    }
};

#ifndef XI_NOW_US_DEFINED
#define XI_NOW_US_DEFINED
// Thin compat aliases over the canonical clock (xi_clock.hpp). now_us() = wall,
// steady_now_us() = monotonic; see xi_clock.hpp for the wall-vs-mono contract.
inline int64_t now_us()        { return xi::wall_us(); }
inline int64_t steady_now_us() { return xi::mono_us(); }
#endif

inline xi_trigger_id make_trigger_id() {
    // 128-bit identifier from a fast TLS PRNG. Not cryptographically random
    // but vanishingly unlikely to collide for any inspection workload.
    thread_local std::mt19937_64 rng{
        std::random_device{}() ^
        (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()};
    xi_trigger_id t{};
    t.hi = rng();
    t.lo = rng();
    if (t.hi == 0 && t.lo == 0) t.lo = 1;     // never collide with NULL
    return t;
}

// Single-sink dispatch ingress: emit_pack funnels in, ONE TriggerEvent per
// emit funnels out to the lone subscribed worker. No routing, no correlation,
// no per-tid grouping (removed in the v6 dispatch cleanup) — despite the name,
// this is a funnel, not a router/bus.
class TriggerBus {
public:
    static TriggerBus& instance() {
        static TriggerBus bus;
        return bus;
    }

    using Sink = std::function<void(TriggerEvent)>;

    // Primary sink: the inspection thread. Only one. Clear via clear_sink.
    void set_sink(Sink sink) {
        std::lock_guard<std::mutex> lk(mu_);
        sink_ = std::move(sink);
    }

    void clear_sink() {
        std::lock_guard<std::mutex> lk(mu_);
        sink_ = nullptr;
    }

    // True if a primary sink is currently installed. Lets a lifecycle op that clears
    // the sink decide whether to RESTORE one on completion (don't fabricate a sink
    // where none existed — e.g. quiescing with no script loaded).
    bool has_sink() {
        std::lock_guard<std::mutex> lk(mu_);
        return static_cast<bool>(sink_);
    }

    // The Pack dispatch ingress. A pack-capable source hands
    // a SEALED pack handle to dispatch. OWNERSHIP IS TRANSFERRED — the caller
    // gives us one ref (the host xi_pack_v1.emit_pack forwarder retained the
    // pack before calling here) and the bus consumes it: it stores the handle on
    // the dispatched event, or releases it via the installed pack releaser when
    // there is no sink. The pack IS the payload — its image entries and metadata
    // doc are read through the Pack accessors, not off the event.
    void emit_pack(const std::string& source,
                    xi_trigger_id id_in,
                    int64_t ts_us,
                    xi_pack_handle pack)
    {
        if (pack == XI_PACK_NULL) return;
        if (ts_us == 0) ts_us = now_us();
        xi_trigger_id id = xi_trigger_id_is_null(id_in) ? make_trigger_id() : id_in;

        // Liveness stamp (monotonic — NTP/DST safe): a source emitting packs is
        // alive, even if there's no sink. Lets dispatch_stats expose "ms since
        // the last frame" globally + per source, so a monitor/FE can detect a
        // CAMERA THAT STALLED. Stamped on every emit, before the sink check.
        {
            int64_t mono = steady_now_us();
            last_emit_mono_us_.store(mono, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(mu_);
            source_last_emit_mono_us_[source] = mono;
        }

        TriggerEvent ev;
        ev.id            = id;
        ev.timestamp_us  = ts_us;
        ev.leader_source = source;
        ev.pack          = pack;    // consume the caller's ref

        Sink to_fire;
        { std::lock_guard<std::mutex> lk(mu_); to_fire = sink_; }

        if (to_fire) {
            to_fire(std::move(ev));
        } else {
            // No sink: hand the ref back to the installed releaser. A REAL pack
            // (non-null — guaranteed by the XI_PACK_NULL guard at entry) arriving
            // here while pack_releaser_ is still null means pack ingress was wired
            // BEFORE set_pack_releaser ran (boot-ordering bug): release_pack_ then
            // silently no-ops and the host PackRegistry ref leaks. Fail loud so the
            // ordering bug surfaces instead of leaking quietly — assert in debug,
            // and a once-only stderr warning in release.
            if (pack_releaser_.load(std::memory_order_acquire) == nullptr) {
                assert(false && "emit_pack: no-sink pack drop with no pack "
                                "releaser installed — host ref leaked; wire "
                                "set_pack_releaser before pack ingress");
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true, std::memory_order_relaxed))
                    std::fprintf(stderr,
                        "[xi_trigger_bus] emit_pack dropped a real pack with no "
                        "releaser installed — host PackRegistry ref leaked; wire "
                        "set_pack_releaser before pack ingress.\n");
            }
            release_pack_(pack);   // no consumer — drop the ref we were handed
        }
    }

    // Install the pack-handle releaser (xi::install_pack_abi wires it to the
    // host PackRegistry). Kept as a bare fn-pointer so this header stays free of
    // any dependency on xi_pack_abi.hpp / the Pack container (it speaks only the
    // opaque xi_pack_handle from xi_abi.h). Null until installed — a bus with no
    // pack plane simply never sees a pack event.
    using PackReleaseFn = void (*)(xi_pack_handle);
    void set_pack_releaser(PackReleaseFn fn) {
        pack_releaser_.store(fn, std::memory_order_release);
    }
    // Release one ref on a pack handle via the installed releaser (no-op if the
    // pack plane was never installed, or the handle is null). Used by the
    // no-sink drop path here and by the dispatcher's release_trigger_event_.
    void release_pack_(xi_pack_handle h) {
        if (h == XI_PACK_NULL) return;
        if (auto fn = pack_releaser_.load(std::memory_order_acquire)) fn(h);
    }

    // Lifecycle reset (script reload / project close). There is no correlation
    // state to drop anymore, but emit_pack() stamps a per-source liveness entry into
    // source_last_emit_mono_us_, and that map otherwise persists for every
    // distinct source name ever seen — an unbounded host leak under
    // rename / create-remove / reopen workflows. The lifecycle callers invoke
    // reset() at exactly the project/script boundary where those source names go
    // out of scope, so prune the map here. Same mutex as the other map accessors.
    void reset() {
        std::lock_guard<std::mutex> lk(mu_);
        source_last_emit_mono_us_.clear();
    }
    // (There is deliberately NO periodic age-based eviction of the per-source
    // liveness map: the map IS the camera-stall signal — source_emit_ages_us
    // reports "µs since last emit" — so evicting a stale entry would delete it
    // precisely when a source has stalled the longest, destroying the very signal
    // it exists to surface. Unbounded growth is bounded instead by reset() at
    // lifecycle boundaries, where source names actually go out of scope. A no-op
    // evict_stale() used to live here purely to satisfy a timer caller; both the
    // method and that call were removed — the timer loop never needed it.)

    // Microseconds since ANY source last emitted (monotonic). -1 if nothing has
    // emitted yet. The "is the line getting frames at all" signal.
    int64_t last_emit_age_us() const {
        int64_t last = last_emit_mono_us_.load(std::memory_order_relaxed);
        return last == 0 ? -1 : (steady_now_us() - last);
    }
    // Per-source age snapshot { source -> µs since its last emit }. Lets a monitor
    // spot which of N cameras stalled.
    std::vector<std::pair<std::string, int64_t>> source_emit_ages_us() {
        std::vector<std::pair<std::string, int64_t>> out;
        std::lock_guard<std::mutex> lk(mu_);
        // Sample the clock AFTER taking the lock (matching last_emit_age_us,
        // which reads the clock after the atomic load). Sampling before the lock
        // races a concurrent emit_pack that stamps source_last_emit_mono_us_ with
        // a mono > our now, yielding a spurious negative age for that source.
        int64_t now = steady_now_us();
        out.reserve(source_last_emit_mono_us_.size());
        for (auto& [s, t] : source_last_emit_mono_us_) out.emplace_back(s, now - t);
        return out;
    }

private:
    std::mutex mu_;
    std::atomic<int64_t> last_emit_mono_us_{0};
    std::unordered_map<std::string, int64_t> source_last_emit_mono_us_;  // guarded by mu_
    Sink       sink_;
    std::atomic<PackReleaseFn> pack_releaser_{nullptr};   // polaris2 wave-2
};

} // namespace xi
