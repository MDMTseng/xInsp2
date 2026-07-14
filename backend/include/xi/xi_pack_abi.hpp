#pragma once
//
// xi_pack_abi.hpp — the HOST side of the xi.pack@1 data-plane door (polaris2
// wave-2, docs/new_gen/07-uniform-keyed-buffer-plane.md + 08 Wave 2).
//
// The Pack value type (xi_pack.hpp) is a C++ container that OWNS a slab and
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
#include "xi_ingress.hpp"   // xi::ingress::canonicalize_into (the foreign-mp gate)
#include "xi_image_pool.hpp" // ImagePool::publish_pack_iface (the door slot)
#include "xi_trigger_bus.hpp"// TriggerBus::emit_pack / set_pack_releaser

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
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
// SINGLE CREATOR TAG (the counted per-owner ledger's replacement). The only
// ref the registry tracks by owner is the CREATOR'S one seal ref: seal() stamps
// the slot with the sealing thread's ImagePool owner and sets creator_ref_live.
// Every other ref (retain / retain_untagged / retain_as) is an untracked ++rc —
// a consumer's ref is its own responsibility. release_as clears the flag when
// the creator drops its own ref (so a later sweep won't double-drop it), and
// the owner sweep (release_all_for) drops AT MOST the creator's ONE seal ref,
// and only iff it is still outstanding — so a sweep is mathematically incapable
// of over-releasing (no clamp needed): a pack a consumer still holds survives
// (rc never reaches 0 under the sweep), a creator that leaked its seal ref is
// reclaimed. A CONSUMER-retain leak is NOT swept — it is a DIAGNOSED leak
// (live_frames() / the teardown diagnostic), reclaimed only at process
// teardown: the registry fails toward LEAK, never toward UAF.
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

    // LOCKING (burr audit K1): mu_ is a shared_mutex so the per-key ABI read
    // trampolines (10+ pack() resolutions per door call) no longer serialize
    // parallel dispatch workers on one global mutex. Classification:
    //   * SHARED  — pure map lookups / read-only scans that touch no slot state:
    //     builder(), pack(), live_frames(), live_builders(), owner_refs().
    //     (Node pointers are stable across concurrent insert/erase of OTHER
    //     handles, and the caller holds a ref on its own handle — see the class
    //     comment — so a resolved pointer stays valid after the lock drops,
    //     exactly as before.)
    //   * UNIQUE  — anything mutating map structure, rc, or the creator tag:
    //     new_builder(), seal(), abandon(), retain_as(), release_as(), untag(),
    //     release_all_for(). rc stays a plain int guarded by the unique lock.
    // ---- builder side (produce) --------------------------------------------
    xi_pack_builder new_builder() {
        uint64_t id = next_.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock<std::shared_mutex> lk(mu_);
        builders_.emplace(id, std::make_unique<PackBuilder>());
        return id;
    }
    PackBuilder* builder(xi_pack_builder b) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = builders_.find(b);
        return it == builders_.end() ? nullptr : it->second.get();
    }
    // Seal + consume: move the builder's pack into the pack table (refcount 1),
    // erase the builder. XI_PACK_NULL on a bad/dead builder id.
    xi_pack_handle seal(xi_pack_builder b) {
        std::unique_ptr<PackBuilder> fb;
        {
            std::unique_lock<std::shared_mutex> lk(mu_);
            auto it = builders_.find(b);
            if (it == builders_.end()) return XI_PACK_NULL;
            fb = std::move(it->second);
            builders_.erase(it);
        }
        uint64_t id = next_.fetch_add(1, std::memory_order_relaxed);
        Slot s;
        s.pack = fb->seal();
        s.rc   = 1;
        // The creator's initial ref is the ONLY owner-tracked ref (seal runs
        // inside the producing plugin's OwnerGuard), so a source that seals and
        // forgets to release is swept on destroy exactly like a leaked image
        // handle. Sealed with no owner context -> no creator tag, never swept.
        s.creator          = ImagePool::current_owner();
        s.creator_ref_live = (s.creator != 0);
        std::unique_lock<std::shared_mutex> lk(mu_);
        frames_.emplace(id, std::move(s));
        return id;
    }
    void abandon(xi_pack_builder b) {
        std::unique_lock<std::shared_mutex> lk(mu_);
        builders_.erase(b);   // ~PackBuilder releases any minted handles
    }

    // ---- pack side (consume) ----------------------------------------------
    Pack* pack(xi_pack_handle f) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = frames_.find(f);
        return it == frames_.end() ? nullptr : &it->second.pack;
    }
    // Retain: an untracked ++rc. A consumer's ref is never owner-tracked (only
    // the creator's seal ref is — see the class comment), so all three retain
    // spellings are identical; they survive as aliases for source compat with
    // the old counted-ledger call sites (adapter, dispatch, tests).
    void retain(xi_pack_handle f) { retain_as(f, 0); }
    void retain_untagged(xi_pack_handle f) { retain_as(f, 0); }
    void retain_as(xi_pack_handle f, ImagePoolOwnerId /*owner*/) {
        std::unique_lock<std::shared_mutex> lk(mu_);   // rc mutation ⇒ unique
        auto it = frames_.find(f);
        if (it == frames_.end()) return;
        ++it->second.rc;
    }
    void release(xi_pack_handle f) { release_as(f, ImagePool::current_owner()); }
    void release_as(xi_pack_handle f, ImagePoolOwnerId owner) {
        Pack dropped;   // destroy OUTSIDE the lock (releases pool handles)
        {
            std::unique_lock<std::shared_mutex> lk(mu_);   // rc + erase ⇒ unique
            auto it = frames_.find(f);
            if (it == frames_.end()) return;
            Slot& s = it->second;
            // The creator releasing its OWN ref, under its own guard, clears
            // the tag so the later teardown sweep won't double-drop it. Owner 0
            // (bus/dispatcher, off-guard threads) never matches a real creator.
            if (s.creator_ref_live && owner == s.creator)
                s.creator_ref_live = false;
            if (--s.rc > 0) return;
            dropped = std::move(s.pack);
            frames_.erase(it);
        }
    }
    // Ownership handoff (the cap/door funnel-output path): the caller now owns
    // what was the creator's seal ref, so clear the creator tag WITHOUT touching
    // rc — the creator's teardown sweep must not reclaim a ref it handed off.
    void untag(xi_pack_handle f, ImagePoolOwnerId owner) {
        std::unique_lock<std::shared_mutex> lk(mu_);   // creator-tag write ⇒ unique
        auto it = frames_.find(f);
        if (it == frames_.end()) return;
        Slot& s = it->second;
        if (s.creator_ref_live && owner == s.creator)
            s.creator_ref_live = false;
    }

    // The owner sweep — the release_all_for analogue for sealed packs. Drops
    // AT MOST ONE ref per slot: the creator's seal ref, iff still outstanding
    // (creator_ref_live). A pack a consumer still holds survives (its rc
    // includes the consumer's untracked ref, so rc cannot reach 0 here); a
    // creator that leaked its seal ref is reclaimed. Mathematically incapable
    // of over-release — no clamp, no surviving-bucket arithmetic. Returns the
    // number of creator refs reclaimed so the caller (adapter dtor / script
    // unload) can print the "swept N leaked pack ref(s)" diagnostic.
    int release_all_for(ImagePoolOwnerId owner) {
        if (owner == 0) return 0;
        std::vector<Pack> dropped;   // destroyed OUTSIDE the lock
        int swept = 0;
        {
            std::unique_lock<std::shared_mutex> lk(mu_);   // rc/tag/erase ⇒ unique
            for (auto it = frames_.begin(); it != frames_.end();) {
                Slot& s = it->second;
                if (s.creator == owner && s.creator_ref_live) {
                    s.creator_ref_live = false;
                    ++swept;
                    if (--s.rc <= 0) {
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
    size_t live_frames()   { std::shared_lock<std::shared_mutex> lk(mu_); return frames_.size(); }
    size_t live_builders() { std::shared_lock<std::shared_mutex> lk(mu_); return builders_.size(); }
    // Diagnostic: live packs whose creator seal ref is still charged to `owner`
    // (the amount a sweep would reclaim right now — at most 1 per slot).
    int owner_refs(ImagePoolOwnerId owner) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        int n = 0;
        for (auto& [id, s] : frames_)
            if (s.creator == owner && s.creator_ref_live) ++n;
        return n;
    }

private:
    struct Slot {
        Pack pack;
        int rc = 0;
        ImagePoolOwnerId creator = 0;   // seal-time owner; 0 = sealed off-guard
        bool creator_ref_live = false;  // creator's seal ref still outstanding
    };

    std::shared_mutex mu_;   // K1: readers shared, mutators unique — see class comment
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
// Finding ⑦: canonicalize foreign msgpack AT THE C-ABI SEAM. Before this fix
// "all foreign msgpack is canonicalized at ingress" was a CONVENTION — only
// ScriptPack::add_mp and xi::ingress gated; this raw trampoline (reached by
// PackOut::mp and any plugin calling builder_add_mp directly) copied caller
// bytes verbatim into the slab via PackBuilder::add_mp (which trusts). Hostile
// or malformed msgpack — including handle-shaped ext (a forged pool ref) — could
// enter a pack entry and travel onto the wire or a replay file. Route it through
// the SAME xi::ingress::canonicalize_into machinery ingress uses (reject-all ext
// policy by default), turning the convention into structure:
//   * already-canonical input re-emits byte-identical (canonicalize is
//     idempotent), so wire/golden bytes are unaffected;
//   * malformed / ext-bearing / non-string-keyed / duplicate-keyed bytes are
//     REFUSED — nothing is stored, matching ScriptPack::add_mp's fail-closed
//     drop. The seam is void (no bool the foreign caller could check), so the
//     refusal is made loud with a warn-once diagnostic instead of a silent drop.
inline void f_add_mp(xi_pack_builder b, const char* key, const void* mp, int32_t len) {
    auto* fb = PackRegistry::instance().builder(b);
    if (!fb) return;
    const size_t n = len > 0 ? (size_t)len : 0;
    if (n == 0 || !mp) { fb->add_mp(key ? key : "", mp, n); return; }  // empty/nil: nothing to validate
    std::span<const uint8_t> foreign(static_cast<const uint8_t*>(mp), n);
    ingress::Result r = ingress::canonicalize_into(*fb, key ? key : "",
                                                   foreign, /*type_tag=*/{});
    if (!r.ok()) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            std::fprintf(stderr,
                "[xinsp2] xi.pack builder_add_mp REFUSED foreign msgpack for key "
                "'%s' (codec status %d, semantic_ok %d) — malformed / ext-bearing "
                "/ non-canonical bytes are rejected at the C-ABI seam, not stored "
                "(finding ⑦). The entry is omitted.\n",
                key ? key : "", (int)r.codec_status, (int)r.semantic_ok);
        }
    }
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
    std::string_view k = fr->key_at((size_t)i);   // borrowed slab bytes, not NUL-terminated
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

// ---- xi.pack@3 trampolines (the slab-generation supplement) -----------------
// Same registry, same handle/builder ids as v1 — @3 only adds verbs. Builder
// verbs return 1 = entry added, 0 = refused (fail-closed, nothing added);
// getters follow v1's 1/0 discipline. dtype/type_id are range-checked HERE
// (the container asserts compile out in Release; the door must stay honest
// against a foreign caller).
inline bool valid_dtype_(int32_t d) {
    return d >= int32_t(PackDtype::U8) && d <= int32_t(PackDtype::F64);
}
inline int32_t f3_add_tensor(xi_pack_builder b, const char* key,
                             int32_t w, int32_t h, int32_t c,
                             int32_t dtype, const void* elems) {
    if (!valid_dtype_(dtype)) return 0;
    auto* fb = PackRegistry::instance().builder(b);
    if (!fb) return 0;
    return fb->add_tensor(key ? key : "", w, h, c, PackDtype(dtype), elems) ? 1 : 0;
}
inline int32_t f3_adopt_tensor(xi_pack_builder b, const char* key,
                               int32_t w, int32_t h, int32_t c,
                               int32_t dtype, xi_image_handle handle) {
    if (!valid_dtype_(dtype)) return 0;
    auto* fb = PackRegistry::instance().builder(b);
    if (!fb) return 0;
    return fb->adopt_tensor(key ? key : "", w, h, c, PackDtype(dtype), handle) ? 1 : 0;
}
inline int32_t f3_add_blob(xi_pack_builder b, const char* key,
                           int32_t type_id, const void* data, int32_t len) {
    if (type_id < XI_PACK_TYPE_USER_BASE || type_id > 0xFFFF || len <= 0) return 0;
    auto* fb = PackRegistry::instance().builder(b);
    if (!fb) return 0;
    return fb->add_blob(key ? key : "", uint16_t(type_id), data, size_t(len)) ? 1 : 0;
}
inline int32_t f3_adopt_bin(xi_pack_builder b, const char* key,
                            int32_t type_id, xi_image_handle handle) {
    if (type_id != 0 && (type_id < XI_PACK_TYPE_USER_BASE || type_id > 0xFFFF))
        return 0;
    auto* fb = PackRegistry::instance().builder(b);
    if (!fb) return 0;
    return fb->adopt_bin(key ? key : "", uint16_t(type_id), handle) ? 1 : 0;
}
inline int32_t f3_get_tensor(xi_pack_handle f, const char* key, xi_pack_tensor* out) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || !key) return 0;
    auto v = fr->get_tensor(key);
    if (!v) return 0;
    if (out) {
        out->width     = v->width;
        out->height    = v->height;
        out->channels  = v->channels;
        out->dtype     = int32_t(v->dtype);
        out->elem_size = int32_t(v->elem_size);
        out->length    = int32_t(v->bytes.size());
        out->data      = v->bytes.data();
        out->handle    = v->handle;
    }
    return 1;
}
inline int32_t f3_get_blob(xi_pack_handle f, const char* key,
                           int32_t* type_id, const void** ptr, int32_t* len) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || !key) return 0;
    auto v = fr->get_blob(key);
    if (!v) return 0;
    if (type_id) *type_id = int32_t(v->type_id);
    if (ptr)     *ptr     = v->bytes.data();
    if (len)     *len     = int32_t(v->bytes.size());
    return 1;
}
inline int32_t f3_type_id_of(xi_pack_handle f, const char* key) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || !key) return -1;
    auto t = fr->type_id_of(key);
    return t ? int32_t(*t) : -1;
}
inline int32_t f3_type_id_at(xi_pack_handle f, int32_t i) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || i < 0 || (size_t)i >= fr->size()) return -1;
    return int32_t(fr->type_id_at((size_t)i));
}
inline int32_t f3_entry_at(xi_pack_handle f, int32_t i, xi_pack_entry* out) {
    Pack* fr = PackRegistry::instance().pack(f);
    if (!fr || i < 0 || (size_t)i >= fr->size()) return 0;
    Pack::EntryView e = fr->entry_at((size_t)i);
    if (out) {
        out->key      = e.key.data();
        out->key_len  = int32_t(e.key.size());
        out->tag      = int32_t(e.tag);
        out->type_id  = int32_t(e.type_id);
        out->external = e.external ? 1 : 0;
    }
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
    // The event's ref is framework-transient — an untracked ++rc, released by
    // the dispatcher on another thread via f_release_for_bus.
    PackRegistry::instance().retain_untagged(f);       // the event's ref
    TriggerBus::instance().emit_pack(emitter ? emitter : "", id, ts, f);
}

// The releaser the bus/dispatcher calls to drop an event's pack ref. Owner 0
// never matches a real creator tag, so this is a plain rc decrement — correct
// for the untagged dispatch-event ref, and it can never clear a creator's tag.
inline void f_release_for_bus(xi_pack_handle f) {
    PackRegistry::instance().release_as(f, 0);
}

// Ownership HANDOFF for a pack a plugin produced FOR ITS CALLER (capability-
// funnel output, pack-door output) — published into ImagePool::pack_untag_slot.
// seal() tags the initial ref to the producing plugin (the creator), but a
// funnel hands that very ref to the CALLER, who owns it. Left creator-tagged,
// the producer's teardown sweep (f_sweep_packs_for → release_all_for) would
// later reclaim the handed-off ref and free the caller's live pack — a UAF /
// wrong-answer. So: clear the creator tag, rc UNCHANGED (PackRegistry::untag).
inline void f_untag_pack_ref(xi_pack_handle f, ImagePoolOwnerId owner) {
    if (f == XI_PACK_NULL) return;
    PackRegistry::instance().untag(f, owner);
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

// The process-stable xi.pack@3 supplement (tensor / user blob / adopt_bin /
// ordinal type_id+entry iteration). Same Meyers-singleton discipline as v1:
// the address and every fn-pointer are stable for the host's life. A consumer
// resolves it ALONGSIDE v1 — lifetime/scalars/emit stay v1 verbs.
inline const xi_pack_v3* pack_v3_iface() {
    static const xi_pack_v3 iface = {
        &pack_abi_detail::f3_add_tensor,
        &pack_abi_detail::f3_adopt_tensor,
        &pack_abi_detail::f3_add_blob,
        &pack_abi_detail::f3_adopt_bin,
        &pack_abi_detail::f3_get_tensor,
        &pack_abi_detail::f3_get_blob,
        &pack_abi_detail::f3_type_id_of,
        &pack_abi_detail::f3_type_id_at,
        &pack_abi_detail::f3_entry_at,
    };
    return &iface;
}

// Publish the Pack data plane so host->get_interface("xi.pack", 1) resolves it
// (and ("xi.pack", 3) the slab supplement), and wire the dispatch releaser so a
// pack event's ref is dropped correctly.
// Idempotent; call once next to install_trigger_hook (default_host_api / certify)
// and in any test that builds a host_api it drives packs through.
inline void install_pack_abi() {
    ImagePool::publish_pack_iface(pack_v1_iface());
    ImagePool::publish_pack3_iface(pack_v3_iface());
    ImagePool::publish_pack_sweeper(&pack_abi_detail::f_sweep_packs_for);
    ImagePool::publish_pack_untagger(&pack_abi_detail::f_untag_pack_ref);
    TriggerBus::instance().set_pack_releaser(&pack_abi_detail::f_release_for_bus);
}

} // namespace xi
