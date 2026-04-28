#pragma once
//
// xi_trigger_bus.hpp — host-side correlator for trigger-tagged frames.
//
// Image-source plugins publish frames via host->emit_trigger(name, tid, ...).
// The bus accumulates per-source images keyed by tid, applies the active
// correlation policy, and dispatches a complete TriggerEvent to the
// subscribed worker (the script's inspect loop).
//
// Policies (set per project at script-load time):
//
//   POLICY_ANY              — fire as soon as any source emits (default,
//                             back-compat: behaves like the old per-source
//                             trigger loop).
//   POLICY_ALL_REQUIRED     — wait until every source in `required_sources`
//                             has emitted for that tid; fire then. Drop
//                             tids that don't complete within window_ms.
//   POLICY_LEADER_FOLLOWERS — fire as soon as the leader emits; attach
//                             whatever the followers have most recently
//                             posted (correlation is best-effort, not
//                             event-locked).
//
// All image handles inside a TriggerEvent are owned by the bus; the worker
// that consumes the event is responsible for releasing them via
// host->image_release() after dispatch.
//

#include "xi_abi.h"
#include "xi_image_pool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xi {

struct TriggerEvent {
    xi_trigger_id  id{0, 0};
    int64_t        timestamp_us = 0;          // earliest source timestamp
    // Source name → image handle. Caller must release each handle after use.
    std::unordered_map<std::string, xi_image_handle> images;
    // Best-effort metadata: which source first published this tid.
    std::string    leader_source;
};

enum class TriggerPolicy {
    Any,                // default; fire on every emit
    AllRequired,        // fire when all required sources have a frame
    LeaderFollowers,    // fire on leader; attach follower latest
};

inline int64_t now_us() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
}

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

    // Primary sink: the inspection thread. Only one. clear via clear_sink.
    void set_sink(Sink sink) {
        std::lock_guard<std::mutex> lk(mu_);
        sink_ = std::move(sink);
    }

    void clear_sink() {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [tid, p] : pending_) {
            for (auto& [src, h] : p.event.images) {
                ImagePool::instance().release(h);
            }
        }
        pending_.clear();
        sink_ = nullptr;
    }

    // Observer sink — fires in addition to the primary sink. Used by the
    // recorder to persist events without interrupting the worker. The
    // observer's event is a deep-ref copy: each image handle is addref'd
    // so the observer can release them on its own schedule.
    void set_observer(Sink observer) {
        std::lock_guard<std::mutex> lk(mu_);
        observer_ = std::move(observer);
    }
    void clear_observer() {
        std::lock_guard<std::mutex> lk(mu_);
        observer_ = nullptr;
    }

    void set_policy(TriggerPolicy policy,
                    std::vector<std::string> required_sources = {},
                    std::string leader_source = "",
                    int window_ms = 100) {
        std::lock_guard<std::mutex> lk(mu_);
        policy_      = policy;
        required_    = std::unordered_set<std::string>(required_sources.begin(),
                                                       required_sources.end());
        leader_      = std::move(leader_source);
        window_ms_   = window_ms;
        // Discard whatever was correlating; any partial events are dropped.
        for (auto& [tid, p] : pending_) {
            for (auto& [src, h] : p.event.images) {
                ImagePool::instance().release(h);
            }
        }
        pending_.clear();
    }

    // Plugins call this via host_api->emit_trigger.
    // The bus addrefs each input handle so the source can release immediately.
    void emit(const std::string& source,
              xi_trigger_id tid_in,
              int64_t ts_us,
              const xi_record_image* images,
              int image_count)
    {
        if (image_count <= 0 || !images) return;
        if (ts_us == 0) ts_us = now_us();
        xi_trigger_id tid = xi_trigger_id_is_null(tid_in) ? make_trigger_id() : tid_in;

        // Addref while still outside the bus lock — handle ops are sharded.
        std::vector<std::pair<std::string, xi_image_handle>> entries;
        entries.reserve(image_count);
        for (int i = 0; i < image_count; ++i) {
            ImagePool::instance().addref(images[i].handle);
            // Use multi-image keys as "<source>/<key>" so a source can
            // publish several named images (e.g. "raw" + "depth"). For the
            // common single-image case the key collapses to source name.
            std::string name = source;
            if (image_count > 1 && images[i].key && images[i].key[0]) {
                name += "/";
                name += images[i].key;
            }
            entries.emplace_back(std::move(name), images[i].handle);
        }

        Sink to_fire;
        TriggerEvent event_to_dispatch;

        {
            std::lock_guard<std::mutex> lk(mu_);
            switch (policy_) {
            case TriggerPolicy::Any: {
                // Build a one-shot event and fire it immediately.
                TriggerEvent ev;
                ev.id = tid;
                ev.timestamp_us = ts_us;
                ev.leader_source = source;
                for (auto& [n, h] : entries) ev.images[n] = h;
                event_to_dispatch = std::move(ev);
                to_fire = sink_;
                break;
            }
            case TriggerPolicy::AllRequired: {
                auto& p = pending_[tid];
                if (p.event.id.hi == 0 && p.event.id.lo == 0) {
                    p.event.id = tid;
                    p.event.timestamp_us = ts_us;
                    p.event.leader_source = source;
                    p.first_seen_us = now_us();
                }
                for (auto& [n, h] : entries) {
                    auto it = p.event.images.find(n);
                    if (it != p.event.images.end()) {
                        // Duplicate from same source? Replace + drop old.
                        ImagePool::instance().release(it->second);
                        it->second = h;
                    } else {
                        p.event.images[n] = h;
                    }
                }
                p.sources_seen.insert(source);
                if (is_complete_locked(p)) {
                    event_to_dispatch = std::move(p.event);
                    pending_.erase(tid);
                    to_fire = sink_;
                }
                evict_stale_locked();
                break;
            }
            case TriggerPolicy::LeaderFollowers: {
                if (source == leader_) {
                    // Build event with leader's image + latest from each follower.
                    TriggerEvent ev;
                    ev.id = tid;
                    ev.timestamp_us = ts_us;
                    ev.leader_source = source;
                    for (auto& [n, h] : entries) ev.images[n] = h;
                    // Attach latest follower frames (still addref'd in the cache)
                    for (auto& [name, h] : follower_latest_) {
                        ImagePool::instance().addref(h);
                        ev.images[name] = h;
                    }
                    event_to_dispatch = std::move(ev);
                    to_fire = sink_;
                } else {
                    // Replace cached latest for this follower-source.
                    for (auto& [n, h] : entries) {
                        auto it = follower_latest_.find(n);
                        if (it != follower_latest_.end()) {
                            ImagePool::instance().release(it->second);
                            it->second = h;
                        } else {
                            follower_latest_[n] = h;
                        }
                    }
                }
                break;
            }
            }
        }

        // Observer gets a deep-ref copy (addref all handles). Dispatched
        // even when no primary sink is set, so recording works before
        // continuous mode starts.
        Sink obs;
        {
            std::lock_guard<std::mutex> lk(mu_);
            obs = observer_;
        }
        if (obs && !event_to_dispatch.images.empty()) {
            TriggerEvent copy;
            copy.id = event_to_dispatch.id;
            copy.timestamp_us = event_to_dispatch.timestamp_us;
            copy.leader_source = event_to_dispatch.leader_source;
            for (auto& [n, h] : event_to_dispatch.images) {
                ImagePool::instance().addref(h);
                copy.images[n] = h;
            }
            try { obs(std::move(copy)); }
            catch (...) { /* observer failures must not kill emit */ }
        }

        // Dispatch to the primary sink (worker/script).
        if (to_fire) to_fire(std::move(event_to_dispatch));
        else {
            for (auto& [n, h] : event_to_dispatch.images) {
                ImagePool::instance().release(h);
            }
        }
    }

    // Drop all pending state. Call on script reload.
    void reset() {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [tid, p] : pending_) {
            for (auto& [src, h] : p.event.images) ImagePool::instance().release(h);
        }
        pending_.clear();
        for (auto& [n, h] : follower_latest_) ImagePool::instance().release(h);
        follower_latest_.clear();
    }

private:
    struct Pending {
        TriggerEvent                       event;
        std::unordered_set<std::string>    sources_seen;
        int64_t                            first_seen_us = 0;
    };

    // Hash + equality for using xi_trigger_id directly as the
    // pending_ map key. Was previously folded to uint64_t via XOR
    // (`hi ^ lo`); two distinct tids whose halves XOR to the same
    // value collided, causing AllRequired to cross-correlate frames
    // from different acquisitions silently.
    struct TidHash {
        std::size_t operator()(const xi_trigger_id& t) const noexcept {
            // 64-bit splitmix-style mix; collisions in the *hash*
            // are fine (map then falls through to TidEq), what
            // matters is no information loss in the KEY.
            uint64_t x = t.hi ^ (t.lo + 0x9E3779B97F4A7C15ull
                + (t.hi << 6) + (t.hi >> 2));
            x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
            x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
            x ^= x >> 33;
            return (std::size_t)x;
        }
    };
    struct TidEq {
        bool operator()(const xi_trigger_id& a, const xi_trigger_id& b) const noexcept {
            return a.hi == b.hi && a.lo == b.lo;
        }
    };

    bool is_complete_locked(const Pending& p) const {
        for (auto& src : required_) {
            if (!p.sources_seen.count(src)) return false;
        }
        return !required_.empty();
    }

    void evict_stale_locked() {
        if (window_ms_ <= 0) return;
        int64_t cutoff = now_us() - (int64_t)window_ms_ * 1000;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.first_seen_us < cutoff) {
                for (auto& [src, h] : it->second.event.images) {
                    ImagePool::instance().release(h);
                }
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::mutex                                  mu_;
    Sink                                        sink_;
    Sink                                        observer_;
    TriggerPolicy                               policy_ = TriggerPolicy::Any;
    std::unordered_set<std::string>             required_;
    std::string                                 leader_;
    int                                         window_ms_ = 100;
    std::unordered_map<xi_trigger_id, Pending, TidHash, TidEq> pending_;
    std::unordered_map<std::string, xi_image_handle> follower_latest_;
};

// Wire emit_trigger on a host_api struct produced by ImagePool::make_host_api().
// Call once after constructing the api; further callers see the live bus.
inline void install_trigger_hook(xi_host_api& api) {
    api.emit_trigger = [](const char* source, xi_trigger_id tid,
                          int64_t ts_us,
                          const xi_record_image* images, int32_t n) {
        TriggerBus::instance().emit(source ? source : "", tid, ts_us, images, n);
    };
}

} // namespace xi
