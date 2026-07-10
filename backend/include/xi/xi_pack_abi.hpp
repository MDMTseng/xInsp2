#pragma once
//
// xi_pack_abi.hpp — the HOST side of the xi.pack@1 data-plane door (polaris2
// wave-2, docs/new_gen/07-uniform-keyed-buffer-plane.md + 08 Wave 2).
//
// The Pack value type (xi_pack.hpp) is a C++ container that OWNS an arena and
// pool handles; a plugin in another DLL cannot touch its layout. So the pack
// crosses the ABI as an OPAQUE HANDLE (xi_pack_handle) plus the accessor C
// functions in `xi_pack_v1` (xi_abi.h) — spans in / spans out, never raw
// struct layout (doc 02 r1). This header is where those C functions live:
//
//   * PackRegistry — the handle table. Maps a handle -> a sealed, refcounted
//     xi::Pack (the pool-handle mint path stays host-side), and a builder
//     handle -> an xi::PackBuilder under construction. Handles are minted ONLY
//     here (doc 07 ingress: the domain's own allocator).
//   * pack_v1_iface() — the process-stable xi_pack_v1 a plugin resolves via
//     host->get_interface("xi.pack", 1). Published into ImagePool's pack slot
//     (the same slot-bridge the emit_record door uses, since image_pool.hpp
//     can't include this header) by install_pack_abi().
//   * emit_pack — a source hands a sealed pack to dispatch; forwarded to
//     TriggerBus::emit_pack (the Record emit path is untouched).
//
// This header sits ABOVE image_pool/trigger_bus in the layering (it includes
// the container). Include it in a .cpp (the backend service, the pack tests),
// never from image_pool.hpp.
//

#include "xi_abi.h"          // xi_pack_handle / xi_pack_v1 / xi_pack_image
#include "xi_pack.hpp"      // xi::Pack / xi::PackBuilder (the container)
#include "xi_image_pool.hpp" // ImagePool::publish_pack_iface (the door slot)
#include "xi_trigger_bus.hpp"// TriggerBus::emit_pack / set_pack_releaser

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace xi {

// ===================================================================
// PackRegistry — the opaque-handle table behind xi.pack@1.
//
// A sealed pack is single-owner in C++ but refcounted across the ABI (an event
// on the dispatch queue and the emitter can each hold a ref, exactly as image
// handles do). So the registry stores each pack with a small refcount and
// destroys it (releasing its pool handles) on the last release. Nodes of a
// std::unordered_map are pointer-stable, so a Pack* handed to an accessor stays
// valid across concurrent insert/erase of OTHER handles — the caller holds a ref
// on its own handle, so that entry cannot vanish under it.
//
// OWNER-TAGGED REFS (pack-plane hardening — the cache plugin's registry
// finding). The ImagePool sweeps leaked image handles per owner on instance
// destroy (release_all_for); the registry used to have NO such analogue, so a
// pack-retaining plugin that forgot to release on destroy leaked its sealed
// packs silently until static teardown. Now every ref acquired under an
// ImagePool owner context (seal / retain inside the adapter's OwnerGuard) is
// tagged in a small per-slot ledger, and release_all_for(owner) drops exactly
// that owner's outstanding refs — precisely as if the plugin had called
// release() itself — returning the count so the teardown path can print the
// "swept N leaked pack ref(s)" diagnostic. Refs acquired with no owner context
// (dispatch-event refs via retain_untagged, test code outside a guard) are
// untagged and never swept. A release under the wrong/no owner context (owner
// 0, or a tagged owner releasing OFF its guard on a worker thread) is
// UNATTRIBUTABLE: it is absorbed by the untagged headroom (rc - sum(buckets))
// when there is any, and otherwise leaves the ledger untouched — it is NEVER
// charged to an arbitrary bucket. (Guessing owners.back() was the R1
// over-release: it could pop a live co-owner's bucket, and the later sweep of
// the mis-charged owner then freed the pack out from under the true holder — a
// UAF.) The sweep (release_all_for) reclaims only the refs NOT attributable to a
// surviving owner and NEVER frees while another owner's bucket is non-empty, so
// **a sweep can never over-release** — a co-owned pack frees only when its true
// last holder releases (rc→0). This is fail-closed toward LEAK, never toward
// UAF: in the narrow double-fault where a surviving owner's bucket is stale
// (that owner released off-guard) AND the swept owner had a forgotten ref, the
// sweep defers the forgotten ref's reclaim (bounded leak, reclaimed at registry
// teardown) rather than risk freeing a live co-owner's pack.
// ===================================================================
class PackRegistry {
public:
    // INTENTIONALLY LEAKED (mirrors ImagePool::instance): process-lifetime
    // state, never destroyed — so late teardown paths (a static-destruction
    // adapter dtor's pack sweep, a late retain/release) can touch the registry
    // unconditionally, with no destroyed-singleton window and no registry-
    // liveness guard flag. The OS reclaims the memory at exit.
    static PackRegistry& instance() {
        static PackRegistry* r = new PackRegistry();
        return *r;
    }

    // ---- builder side (produce) --------------------------------------------
    xi_pack_builder new_builder() {
        uint64_t id = next_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mu_);
        builders_.emplace(id, std::make_unique<PackBuilder>());
        return id;
    }
    PackBuilder* builder(xi_pack_builder b) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = builders_.find(b);
        return it == builders_.end() ? nullptr : it->second.get();
    }
    // Seal + consume: move the builder's pack into the pack table (refcount 1),
    // erase the builder. XI_PACK_NULL on a bad/dead builder id.
    xi_pack_handle seal(xi_pack_builder b) {
        std::unique_ptr<PackBuilder> fb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = builders_.find(b);
            if (it == builders_.end()) return XI_PACK_NULL;
            fb = std::move(it->second);
            builders_.erase(it);
        }
        uint64_t id = next_.fetch_add(1, std::memory_order_relaxed);
        Slot s;
        s.pack = fb->seal();
        s.rc   = 1;
        // The creator's initial ref is owner-tagged (seal runs inside the
        // producing plugin's OwnerGuard), so a source that seals and forgets
        // to release is swept on destroy exactly like a leaked image handle.
        if (ImagePoolOwnerId ow = ImagePool::current_owner())
            s.owners.push_back(OwnerRef{ow, 1});
        std::lock_guard<std::mutex> lk(mu_);
        frames_.emplace(id, std::move(s));
        return id;
    }
    void abandon(xi_pack_builder b) {
        std::lock_guard<std::mutex> lk(mu_);
        builders_.erase(b);   // ~PackBuilder releases any minted handles
    }

    // ---- pack side (consume) ----------------------------------------------
    Pack* pack(xi_pack_handle f) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = frames_.find(f);
        return it == frames_.end() ? nullptr : &it->second.pack;
    }
    // Owner-tagged retain: the ref is charged to the calling thread's ImagePool
    // owner context (the adapter's OwnerGuard around every plugin entry point),
    // so release_all_for(owner) knows it is outstanding if the plugin forgets.
    void retain(xi_pack_handle f) { retain_as(f, ImagePool::current_owner()); }
    // Untagged retain — for framework-internal transient refs (the dispatch
    // event's ref in f_emit_pack) that are released from OTHER threads with no
    // owner context and must never be charged to (or swept with) the plugin.
    void retain_untagged(xi_pack_handle f) { retain_as(f, 0); }
    void retain_as(xi_pack_handle f, ImagePoolOwnerId owner) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = frames_.find(f);
        if (it == frames_.end()) return;
        ++it->second.rc;
        if (owner) ledger_bump(it->second, owner);
    }
    void release(xi_pack_handle f) { release_as(f, ImagePool::current_owner()); }
    void release_as(xi_pack_handle f, ImagePoolOwnerId owner) {
        Pack dropped;   // destroy OUTSIDE the lock (releases pool handles)
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = frames_.find(f);
            if (it == frames_.end()) return;
            ledger_release(it->second, owner);
            if (--it->second.rc > 0) return;
            dropped = std::move(it->second.pack);
            frames_.erase(it);
        }
    }

    // The owner sweep — the release_all_for analogue for sealed packs. Drops
    // every ref still charged to `owner` in the ledger (exactly as if the dying
    // plugin had called release() per outstanding ref), destroying any pack
    // whose count reaches zero. Returns the number of leaked refs dropped so
    // the caller (adapter dtor / script unload) can print the diagnostic.
    // A ref another consumer holds is untouched — the ledger sum never exceeds
    // the slot's refcount (see ledger_release), so the sweep cannot free a pack
    // out from under a live co-owner.
    int release_all_for(ImagePoolOwnerId owner) {
        if (owner == 0) return 0;
        std::vector<Pack> dropped;   // destroyed OUTSIDE the lock
        int swept = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto it = frames_.begin(); it != frames_.end();) {
                Slot& s = it->second;
                int k = ledger_take(s, owner);   // remove this owner's bucket
                if (k > 0) {
                    // R1 guard: reclaim only the refs NOT attributable to a
                    // surviving owner. A survivor Y's genuine holds are <= its
                    // bucket, so anything beyond sum(remaining buckets) belongs
                    // to THIS dying owner (a forgotten/leaked ref) and is safe to
                    // drop. Never reclaim more than this owner's own bucket, and
                    // NEVER free while another owner's bucket is non-empty — a
                    // co-owned pack frees when the last co-owner releases (rc→0),
                    // so the sweep can never over-release the true holder.
                    int remaining = 0;
                    for (const OwnerRef& r : s.owners) remaining += r.n;
                    int reclaim = s.rc - remaining;
                    if (reclaim < 0) reclaim = 0;
                    if (reclaim > k) reclaim = k;
                    swept += reclaim;
                    s.rc -= reclaim;
                    if (s.rc <= 0 && s.owners.empty()) {
                        dropped.push_back(std::move(s.pack));
                        it = frames_.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }
        return swept;
    }

    // Test/diagnostic: how many live packs + builders the table holds. Used by
    // the pack-door tests as a leak oracle alongside ImagePool::cumulative().
    size_t live_frames()   { std::lock_guard<std::mutex> lk(mu_); return frames_.size(); }
    size_t live_builders() { std::lock_guard<std::mutex> lk(mu_); return builders_.size(); }
    // Diagnostic: refs currently charged to `owner` across all live packs (the
    // amount a sweep would reclaim right now).
    int owner_refs(ImagePoolOwnerId owner) {
        std::lock_guard<std::mutex> lk(mu_);
        int n = 0;
        for (auto& [id, s] : frames_)
            for (const OwnerRef& r : s.owners)
                if (r.owner == owner) n += r.n;
        return n;
    }

private:
    struct OwnerRef { ImagePoolOwnerId owner; int n; };
    struct Slot {
        Pack pack;
        int rc = 0;
        std::vector<OwnerRef> owners;   // per-owner outstanding-ref ledger
    };

    static void ledger_bump(Slot& s, ImagePoolOwnerId owner) {
        for (OwnerRef& r : s.owners)
            if (r.owner == owner) { ++r.n; return; }
        s.owners.push_back(OwnerRef{owner, 1});
    }
    // Reconcile one release against the ledger BEFORE rc is decremented.
    //   1. the caller's own bucket -> decrement it (the well-behaved case);
    //   2. no matching bucket -> the ref is untagged, or a tagged ref released
    //      OFF its owner guard (owner 0, or an owner with no bucket). Either way
    //      it is UNATTRIBUTABLE: leave the ledger untouched. The rc decrement in
    //      release_as records the drop; the sweep reclaims only refs beyond the
    //      surviving buckets. We do NOT guess a bucket — the old code decremented
    //      owners.back() once the untagged headroom (rc - sum(buckets)) was
    //      exhausted, which could pop a LIVE co-owner's bucket and let a later
    //      sweep of the mis-charged owner over-release the pack (R1 UAF).
    static void ledger_release(Slot& s, ImagePoolOwnerId owner) {
        if (owner) {
            for (size_t i = 0; i < s.owners.size(); ++i) {
                if (s.owners[i].owner == owner) {
                    if (--s.owners[i].n == 0) {
                        s.owners[i] = s.owners.back();
                        s.owners.pop_back();
                    }
                    return;
                }
            }
        }
        // Unattributable release: never mis-charge another owner's bucket.
    }
    // Remove and return owner's whole bucket (the sweep primitive).
    static int ledger_take(Slot& s, ImagePoolOwnerId owner) {
        for (size_t i = 0; i < s.owners.size(); ++i) {
            if (s.owners[i].owner == owner) {
                int n = s.owners[i].n;
                s.owners[i] = s.owners.back();
                s.owners.pop_back();
                return n;
            }
        }
        return 0;
    }
    std::mutex mu_;
    std::unordered_map<uint64_t, std::unique_ptr<PackBuilder>> builders_;
    std::unordered_map<uint64_t, Slot> frames_;
    std::atomic<uint64_t> next_{1};
};

namespace pack_abi_detail {

inline PackTag tag_from_int(int32_t t) { return static_cast<PackTag>(t); }

// ---- builder trampolines ---------------------------------------------------
inline xi_pack_builder f_builder_new() { return PackRegistry::instance().new_builder(); }
inline void f_add_i64(xi_pack_builder b, const char* key, int64_t v) {
    if (auto* fb = PackRegistry::instance().builder(b)) fb->add_i64(key ? key : "", v);
}
inline void f_add_f64(xi_pack_builder b, const char* key, double v) {
    if (auto* fb = PackRegistry::instance().builder(b)) fb->add_f64(key ? key : "", v);
}
inline void f_add_bool(xi_pack_builder b, const char* key, int32_t v) {
    if (auto* fb = PackRegistry::instance().builder(b)) fb->add_bool(key ? key : "", v != 0);
}
inline void f_add_str(xi_pack_builder b, const char* key, const char* s, int32_t len) {
    if (auto* fb = PackRegistry::instance().builder(b))
        fb->add_str(key ? key : "", std::string_view(s ? s : "", len > 0 ? (size_t)len : 0));
}
inline void f_add_bin(xi_pack_builder b, const char* key, const void* data, int32_t len) {
    if (auto* fb = PackRegistry::instance().builder(b))
        fb->add_bin(key ? key : "", data, len > 0 ? (size_t)len : 0);
}
inline void f_add_image(xi_pack_builder b, const char* key,
                        int32_t w, int32_t h, int32_t c, const void* px) {
    if (auto* fb = PackRegistry::instance().builder(b)) fb->add_image(key ? key : "", w, h, c, px);
}
inline void f_adopt_image(xi_pack_builder b, const char* key,
                          int32_t w, int32_t h, int32_t c, xi_image_handle handle) {
    if (auto* fb = PackRegistry::instance().builder(b))
        fb->adopt_image(key ? key : "", w, h, c, handle);
}
inline void f_add_mp(xi_pack_builder b, const char* key, const void* mp, int32_t len) {
    if (auto* fb = PackRegistry::instance().builder(b))
        fb->add_mp(key ? key : "", mp, len > 0 ? (size_t)len : 0);
}
inline xi_pack_handle f_seal(xi_pack_builder b) { return PackRegistry::instance().seal(b); }
inline void f_abandon(xi_pack_builder b) { PackRegistry::instance().abandon(b); }

// ---- accessor trampolines --------------------------------------------------
inline int32_t f_count(xi_pack_handle f) {
    Pack* fr = PackRegistry::instance().pack(f);
    return fr ? (int32_t)fr->size() : 0;
}
inline const char* f_key_at(xi_pack_handle f, int32_t i, int32_t* len) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || i < 0 || (size_t)i >= fr->size()) { if (len) *len = 0; return nullptr; }
    std::string_view k = fr->key_at((size_t)i);   // borrowed arena bytes, not NUL-terminated
    if (len) *len = (int32_t)k.size();
    return k.data();
}
inline int32_t f_tag_at(xi_pack_handle f, int32_t i) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || i < 0 || (size_t)i >= fr->size()) return -1;
    return (int32_t)fr->tag_at((size_t)i);
}
inline int32_t f_tag_of(xi_pack_handle f, const char* key) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || !key) return -1;
    auto t = fr->tag_of(key);
    return t ? (int32_t)*t : -1;
}
inline int32_t f_get_i64(xi_pack_handle f, const char* key, int64_t* out) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || !key) return 0;
    auto v = fr->get_i64(key);
    if (!v) return 0;
    if (out) *out = *v;
    return 1;
}
inline int32_t f_get_f64(xi_pack_handle f, const char* key, double* out) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || !key) return 0;
    auto v = fr->get_f64(key);
    if (!v) return 0;
    if (out) *out = *v;
    return 1;
}
inline int32_t f_get_bool(xi_pack_handle f, const char* key, int32_t* out) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || !key) return 0;
    auto v = fr->get_bool(key);
    if (!v) return 0;
    if (out) *out = *v ? 1 : 0;
    return 1;
}
inline int32_t f_get_str(xi_pack_handle f, const char* key, const char** ptr, int32_t* len) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || !key) return 0;
    auto v = fr->get_str(key);
    if (!v) return 0;
    if (ptr) *ptr = v->data();
    if (len) *len = (int32_t)v->size();
    return 1;
}
inline int32_t f_get_bin(xi_pack_handle f, const char* key, const void** ptr, int32_t* len) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || !key) return 0;
    auto v = fr->get_bin(key);
    if (!v) return 0;
    if (ptr) *ptr = v->data();
    if (len) *len = (int32_t)v->size();
    return 1;
}
inline int32_t f_get_image(xi_pack_handle f, const char* key, xi_pack_image* out) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || !key) return 0;
    auto v = fr->get_image(key);
    if (!v) return 0;
    if (out) {
        out->width    = v->width;
        out->height   = v->height;
        out->channels = v->channels;
        out->pixels   = v->pixels.data();
        out->length   = (int32_t)v->pixels.size();
    }
    return 1;
}
inline int32_t f_get_mp(xi_pack_handle f, const char* key, const void** ptr, int32_t* len) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || !key) return 0;
    auto v = fr->get_mp(key);
    if (!v) return 0;
    if (ptr) *ptr = v->data();
    if (len) *len = (int32_t)v->size();
    return 1;
}

// ---- lifetime / emit -------------------------------------------------------
// The registry singleton is intentionally leaked (see PackRegistry::instance),
// so a late retain/release during static destruction is safe unconditionally —
// no alive-flag guard on the per-frame refcount hot path.
inline void f_retain(xi_pack_handle f) {
    PackRegistry::instance().retain(f);
}
inline void f_release(xi_pack_handle f) {
    PackRegistry::instance().release(f);
}

// The emit_pack forwarder. The caller (an SDK source) still holds its own ref;
// we take a SECOND ref for the async dispatch event and hand it to the bus,
// which consumes it (stores it on the event or releases it if there's no sink).
inline void f_emit_pack(const char* emitter, xi_trigger_id id,
                         xi_pack_handle f, int64_t ts) {
    if (f == XI_PACK_NULL) return;
    // UNTAGGED: the event's ref is framework-transient — released by the
    // dispatcher on another thread with no owner context. Tagging it to the
    // emitting plugin would leave a phantom ledger entry the owner sweep
    // could later double-release.
    PackRegistry::instance().retain_untagged(f);       // the event's ref
    TriggerBus::instance().emit_pack(emitter ? emitter : "", id, ts, f);
}

// The releaser the bus/dispatcher calls to drop an event's pack ref.
// UNTAGGED (owner 0) to pair with f_emit_pack's retain_untagged: the dispatcher
// runs with no owner context, and an owner-tagged release here would mis-charge
// the ambient plugin's ledger bucket and skew the later owner sweep.
inline void f_release_for_bus(xi_pack_handle f) {
    PackRegistry::instance().release_as(f, 0);
}

// Ownership HANDOFF for a pack a plugin produced FOR ITS CALLER (capability-
// funnel output, pack-door output) — published into ImagePool::pack_untag_slot.
// seal() owner-tags the initial ref to the thread's current owner (the
// producing plugin's OwnerGuard), but a funnel hands that very ref to the
// CALLER, who owns it. Left producer-tagged, the ledger carries a PHANTOM
// {producer,1} charge for a ref the consumer legitimately holds, and the
// producer's teardown sweep (f_sweep_packs_for → release_all_for) would later
// reclaim it and free the caller's live pack — a UAF / wrong-answer. Same
// doctrine as f_emit_pack's retain_untagged above: a ref that outlives the
// producing plugin must not sit in its ledger bucket. Retain FIRST so the
// refcount never transits zero: +1 untagged, then -1 popping the producer-
// tagged ref — net refcount unchanged, tag moved off the producer.
inline void f_untag_pack_ref(xi_pack_handle f, ImagePoolOwnerId owner) {
    if (f == XI_PACK_NULL) return;
    PackRegistry::instance().retain_untagged(f);
    PackRegistry::instance().release_as(f, owner);
}

// The owner-sweep trampoline published into ImagePool::pack_sweep_slot, so the
// teardown paths that already sweep leaked image handles (adapter dtor, script
// unload) reclaim leaked pack refs through the same layering bridge. Safe even
// during static destruction: the registry singleton is intentionally leaked.
inline int f_sweep_packs_for(ImagePoolOwnerId owner) {
    return PackRegistry::instance().release_all_for(owner);
}

} // namespace pack_abi_detail

// The process-stable xi.pack@1 data-plane interface. Meyers singleton, so its
// address (and every fn-pointer) is stable for the host's life — a plugin caches
// the pointer once (Plugin::pack_iface()).
inline const xi_pack_v1* pack_v1_iface() {
    static const xi_pack_v1 iface = {
        &pack_abi_detail::f_builder_new,
        &pack_abi_detail::f_add_i64,
        &pack_abi_detail::f_add_f64,
        &pack_abi_detail::f_add_str,
        &pack_abi_detail::f_add_bin,
        &pack_abi_detail::f_add_image,
        &pack_abi_detail::f_adopt_image,
        &pack_abi_detail::f_add_mp,
        &pack_abi_detail::f_seal,
        &pack_abi_detail::f_abandon,
        &pack_abi_detail::f_count,
        &pack_abi_detail::f_key_at,
        &pack_abi_detail::f_tag_at,
        &pack_abi_detail::f_tag_of,
        &pack_abi_detail::f_get_i64,
        &pack_abi_detail::f_get_f64,
        &pack_abi_detail::f_get_str,
        &pack_abi_detail::f_get_bin,
        &pack_abi_detail::f_get_image,
        &pack_abi_detail::f_get_mp,
        &pack_abi_detail::f_retain,
        &pack_abi_detail::f_release,
        &pack_abi_detail::f_emit_pack,
        // additive v1 tail (bool entry) — positions match the struct tail.
        &pack_abi_detail::f_add_bool,
        &pack_abi_detail::f_get_bool,
    };
    return &iface;
}

// Publish the Pack data plane so host->get_interface("xi.pack", 1) resolves it,
// and wire the dispatch releaser so a pack event's ref is dropped correctly.
// Idempotent; call once next to install_trigger_hook (default_host_api / certify)
// and in any test that builds a host_api it drives packs through.
inline void install_pack_abi() {
    ImagePool::publish_pack_iface(pack_v1_iface());
    ImagePool::publish_pack_sweeper(&pack_abi_detail::f_sweep_packs_for);
    ImagePool::publish_pack_untagger(&pack_abi_detail::f_untag_pack_ref);
    TriggerBus::instance().set_pack_releaser(&pack_abi_detail::f_release_for_bus);
}

} // namespace xi
