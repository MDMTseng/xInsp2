#pragma once
//
// xi_trigger_bus.hpp — the host-side dispatch funnel for emit_record.
//
// A source plugin pushes a record (frames + metadata) via host->emit_record
// (xi::emit_record). The bus builds ONE TriggerEvent per emit and hands it to
// the subscribed worker (the script's inspect loop). There is no correlation:
// a source that wants several frames inspected together (e.g. a gathering
// stereo source) puts them in the SAME record, so the bus is a pure funnel —
// emit in, dispatch out. (Multi-camera sync, recording/replay, and trigger
// policies were removed in the ABI-v6 dispatch cleanup: sync is a gathering
// plugin, replay is a buffer-replay plugin.)
//
// Every image handle inside a TriggerEvent is owned by the bus; the worker that
// consumes the event releases them (and the metadata doc) after dispatch.
//

#include "xi_abi.h"
#include "xi_clock.hpp"
#include "xi_doc_registry.hpp"
#include "xi_image_pool.hpp"

#include <atomic>
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
    // Image key → handle. The key is the record's own key whenever the source
    // supplied one (e.g. "cam_left"/"cam_right", or "frame" for a single image
    // emitted as Record().image("frame", img)); a keyless image falls back to
    // the emitter name. A single-image event additionally resolves by ANY key
    // via the reader-side fallback (see trigger_image_cb). Caller releases each
    // handle after use.
    std::unordered_map<std::string, xi_image_handle> images;
    // The emitting instance's name — current_trigger().primary_source().
    std::string    leader_source;
    // Dispatch group (priority/concurrency lane). Stamped by the dispatcher sink
    // from the emitting instance's "group" (default_group if untagged).
    std::string    group;
    // ABI v6: routing/context metadata from emit_record, carried by POINTER
    // (zero-serialize) across the async dispatch — a host-owned yyjson_mut_doc*
    // refcounted through DocRegistry, exactly as the image handles ride the
    // ImagePool refcount. NULL when the record carried no metadata. Ownership is
    // now TYPED: a move-only DocRef owns the event's one registry ref and releases
    // it in its destructor, so the ref can't leak or double-free across the async
    // dispatch. release_trigger_event_() still reset()s it at the drop/consume site
    // (releasing the images alongside); the destructor is the backstop. The script
    // reads it as a borrowed read-only view via current_trigger().meta().
    DocRef meta_doc;
    // polaris2 wave-2: OPTIONAL Frame carry (transitional dual-carry, docs/new_gen/
    // 07 + 08 Wave 2). When set, this event rides the v3 keyed-buffer Frame plane
    // (a sealed, immutable, refcounted host frame handle) instead of / alongside
    // the Record's images+meta_doc — the ordering/EmitGate machinery keys on id +
    // arrival_id, so it carries a frame identically. The bus took ONE ref on
    // emit_frame; release_trigger_event_() drops it at the drop/consume site via
    // TriggerBus::release_frame_(). XI_FRAME_NULL for every Record-era event, so
    // the Record path stays byte-for-byte unchanged.
    xi_frame_handle frame = XI_FRAME_NULL;
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

// Single-sink dispatch ingress: emit_record funnels in, ONE TriggerEvent per
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

    // emit_record routes here. meta_doc (ABI v6): the event's metadata doc, a
    // host-owned yyjson_mut_doc*. OWNERSHIP IS TRANSFERRED — the caller hands
    // one ref and the bus consumes it (stores it on the dispatched event, or
    // DocRegistry::release()s it when there is no sink). nullptr ⇒ no metadata.
    // The bus addrefs each input image handle so the source can release at once.
    void emit(const std::string& source,
              xi_trigger_id id_in,
              int64_t ts_us,
              const xi_record_image* images,
              int image_count,
              yyjson_mut_doc* meta_doc = nullptr)
    {
        // We own the transferred meta_doc ref: every return path releases it.
        if (image_count <= 0 || !images) {
            DocRegistry::instance().release(meta_doc);
            return;
        }
        if (ts_us == 0) ts_us = now_us();
        xi_trigger_id id = xi_trigger_id_is_null(id_in) ? make_trigger_id() : id_in;

        // Liveness stamp (monotonic — NTP/DST safe): a source emitting is alive,
        // even if there's no sink. Lets dispatch_stats expose "ms since the last
        // frame" globally + per source, so a monitor/FE can detect a CAMERA THAT
        // STALLED — which otherwise stops the line with zero signal (nothing else
        // tracked last-emit). Stamped on every emit, before the sink check.
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
        ev.meta_doc      = DocRef::adopt(meta_doc);   // transfer the caller's ref (no retain)
        for (int i = 0; i < image_count; ++i) {
            xi_image_handle h = images[i].handle;
            ImagePool::instance().addref(h);
            // Key every image by the record's OWN key when the source gave one
            // (multi-image "cam_left"/"cam_right", AND a single image emitted as
            // Record().image("frame", img) — the documented contract that
            // cmd:run/replay also use). A keyless image collapses to the emitter
            // name so legacy t.image("<source>") still works. Choosing the key is
            // free — still exactly ONE string per image, no extra map entry; the
            // reader-side sole-image fallback (trigger_image_cb) lets a single
            // frame resolve by EITHER the record key or the instance name.
            std::string name;
            if (images[i].key && images[i].key[0]) {
                name = images[i].key;
            } else {
                name = source;
            }
            // A duplicate (or empty -> source) key in a multi-image record
            // collides on the map: emplace fails and keeps the first handle.
            // release_trigger_event_ only iterates STORED handles, so release the
            // ref we just added or it leaks an ImagePool slot. Warn once so the
            // offending source gets distinct keys.
            if (!ev.images.emplace(std::move(name), h).second) {
                ImagePool::instance().release(h);
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true, std::memory_order_relaxed))
                    std::fprintf(stderr, "[xinsp2] emit('%s'): duplicate/empty image key "
                                 "dropped — a multi-image record needs distinct keys\n",
                                 source.c_str());
            }
        }

        Sink to_fire;
        { std::lock_guard<std::mutex> lk(mu_); to_fire = sink_; }

        if (to_fire) {
            to_fire(std::move(ev));
        } else {
            for (auto& [n, h] : ev.images) ImagePool::instance().release(h);
            ev.meta_doc.reset();   // release the event's doc ref (dtor would too)
        }
    }

    // polaris2 wave-2: the Frame sibling of emit(). A frame-capable source hands
    // a SEALED frame handle to dispatch. OWNERSHIP IS TRANSFERRED — the caller
    // gives us one ref (the host xi_frame_v1.emit_frame forwarder retained the
    // frame before calling here) and the bus consumes it: it stores the handle on
    // the dispatched event, or releases it via the installed frame releaser when
    // there is no sink. Same funnel discipline as emit(); the Record path is
    // untouched. Images are NOT extracted onto the event — the frame IS the
    // payload (a frame's image entries are read through the Frame accessors, not
    // the event's image map).
    void emit_frame(const std::string& source,
                    xi_trigger_id id_in,
                    int64_t ts_us,
                    xi_frame_handle frame)
    {
        if (frame == XI_FRAME_NULL) return;
        if (ts_us == 0) ts_us = now_us();
        xi_trigger_id id = xi_trigger_id_is_null(id_in) ? make_trigger_id() : id_in;

        // Liveness stamp — identical to emit(): a source emitting frames is alive.
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
        ev.frame         = frame;   // consume the caller's ref

        Sink to_fire;
        { std::lock_guard<std::mutex> lk(mu_); to_fire = sink_; }

        if (to_fire) {
            to_fire(std::move(ev));
        } else {
            release_frame_(frame);   // no consumer — drop the ref we were handed
        }
    }

    // Install the frame-handle releaser (xi::install_frame_abi wires it to the
    // host FrameRegistry). Kept as a bare fn-pointer so this header stays free of
    // any dependency on xi_frame_abi.hpp / the Frame container (it speaks only the
    // opaque xi_frame_handle from xi_abi.h). Null until installed — a bus with no
    // frame plane simply never sees a frame event.
    using FrameReleaseFn = void (*)(xi_frame_handle);
    void set_frame_releaser(FrameReleaseFn fn) {
        frame_releaser_.store(fn, std::memory_order_release);
    }
    // Release one ref on a frame handle via the installed releaser (no-op if the
    // frame plane was never installed, or the handle is null). Used by the
    // no-sink drop path here and by the dispatcher's release_trigger_event_.
    void release_frame_(xi_frame_handle h) {
        if (h == XI_FRAME_NULL) return;
        if (auto fn = frame_releaser_.load(std::memory_order_acquire)) fn(h);
    }

    // Lifecycle reset (script reload / project close). There is no correlation
    // state to drop anymore, but emit() stamps a per-source liveness entry into
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
        int64_t now = steady_now_us();
        std::lock_guard<std::mutex> lk(mu_);
        out.reserve(source_last_emit_mono_us_.size());
        for (auto& [s, t] : source_last_emit_mono_us_) out.emplace_back(s, now - t);
        return out;
    }

private:
    std::mutex mu_;
    std::atomic<int64_t> last_emit_mono_us_{0};
    std::unordered_map<std::string, int64_t> source_last_emit_mono_us_;  // guarded by mu_
    Sink       sink_;
    std::atomic<FrameReleaseFn> frame_releaser_{nullptr};   // polaris2 wave-2
};

// Wire emit_record on a host_api struct produced by ImagePool::make_host_api().
// Call once after constructing the api; further callers see the live bus.
inline void install_trigger_hook(xi_host_api& api) {
    // ABI v6: emit_record — the one dispatch verb. The record's images +
    // metadata doc go onto the dispatch path; we hand emit() ONE owned ref to
    // the meta doc (it consumes it). rec->doc arrives through the SDK's
    // share_out (xi::emit_record) exactly like the process() output doc:
    // share_out reserved one registry ref for the consumer, so we CONSUME it
    // here (no extra retain) — adopt_shared's analogue. A doc-less record with
    // raw JSON bytes is parsed once into a fresh host-owned doc (the only
    // deserialize, taken only when a hand-rolled source chose data/len).
    api.emit_record = [](const char* emitter, xi_trigger_id id,
                         const struct xi_record* rec, int64_t ts) {
        if (!rec) return;
        yyjson_mut_doc* meta = nullptr;
        if (rec->doc) {
            // share_out reserved a ref for us; take it as the host's ref.
            meta = (yyjson_mut_doc*)(void*)rec->doc;
        } else if (rec->data && rec->len > 0) {
            yyjson_doc* idoc = yyjson_read((const char*)rec->data, (size_t)rec->len, 0);
            if (idoc) {
                meta = yyjson_doc_mut_copy(idoc, nullptr);   // host-owned (default alc)
                yyjson_doc_free(idoc);
                if (meta) DocRegistry::instance().addref(meta);   // register at rc=1
            }
        }
        TriggerBus::instance().emit(emitter ? emitter : "", id, ts,
                                    rec->images, rec->image_count, meta);
    };
    // ABI v11: publish the wired emit_record into the process-global slot the
    // carved xi.emit@1 interface's forwarder reads (xi_image_pool.hpp). This is
    // the layering bridge: image_pool.hpp cannot include this header, so the door
    // reaches the live dispatch verb only once we hand it the pointer here — so
    // the door's emit_record and the struct field host->emit_record hit the SAME
    // code path (freeze-guard: ImagePool::door_matches_fields).
    ImagePool::publish_emit_record(api.emit_record);
}

} // namespace xi
