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
    // Image key → handle. The key is the record's own key for a multi-image
    // record (e.g. "cam_left"/"cam_right"), or the emitter name for a single
    // image. Caller releases each handle after use.
    std::unordered_map<std::string, xi_image_handle> images;
    // The emitting instance's name — current_trigger().primary_source().
    std::string    leader_source;
    // Dispatch group (priority/concurrency lane). Stamped by the dispatcher sink
    // from the emitting instance's "group" (default_group if untagged).
    std::string    group;
    // ABI v6: routing/context metadata from emit_record, carried by POINTER
    // (zero-serialize) across the async dispatch — a host-owned yyjson_mut_doc*
    // refcounted through DocRegistry, exactly as the image handles ride the
    // ImagePool refcount. NULL when the record carried no metadata. Owned like
    // the images: whoever holds this event owns one ref and MUST
    // DocRegistry::release() it at every drop/consume site (TriggerEvent has no
    // destructor — the discipline is manual). The script reads it as a borrowed
    // read-only view via current_trigger().meta().
    yyjson_mut_doc* meta_doc = nullptr;
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
        // STALLED — which otherwise stops the line with zero signal (the timer's
        // evict_stale was a no-op and nothing tracked last-emit). Stamped on every
        // emit, before the sink check.
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
        ev.meta_doc      = meta_doc;            // transfer
        for (int i = 0; i < image_count; ++i) {
            xi_image_handle h = images[i].handle;
            ImagePool::instance().addref(h);
            // A multi-image record is keyed by the record's OWN keys, so the
            // script reads t.image("cam_left") directly. A single image
            // collapses to the emitter name, so t.image("<source>") works.
            std::string name;
            if (image_count > 1 && images[i].key && images[i].key[0]) {
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
            DocRegistry::instance().release(ev.meta_doc);
        }
    }

    // No correlation state to drop anymore; kept as no-ops so the lifecycle
    // callers (script reload / project close) and the dispatch timer's periodic
    // call in service_main need not change.
    void reset() {}
    void evict_stale() {}

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
                if (meta) DocRegistry::instance().retain(meta);   // register at rc=1
            }
        }
        TriggerBus::instance().emit(emitter ? emitter : "", id, ts,
                                    rec->images, rec->image_count, meta);
    };
}

} // namespace xi
