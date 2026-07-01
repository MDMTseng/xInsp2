#pragma once
//
// xi_doc_registry.hpp — host-side refcount for yyjson docs shared across the
// plugin ABI (γ-4). The doc analogue of ImagePool's per-handle refcount:
// a yyjson_mut_doc* handed across the boundary can be retained by more than
// one side (the host adopting a doc a plugin still caches; a plugin retaining
// its borrowed input), and the registry owns it — freeing it
// (yyjson_mut_doc_free, which routes chunks back via the doc's own host
// allocator) only when the last side releases.
//
// Backs host_api.doc_retain / doc_release (xi_abi.h v4). This is the single,
// uniform path for every doc that crosses the boundary — emit retains, the
// other side adopt_shared's, each drop releases — exactly like image_addref/
// adopt/release. To keep that uniform path cheap under parallel dispatch
// (multiple workers emitting at once), the map is SHARDED by doc pointer: each
// shard has its own mutex, so concurrent retains/releases on different docs
// don't contend. A doc op is hash(ptr) → one shard → one short critical section.
//
// TODO(linux): std::mutex + unordered_map only — already cross-platform.
//

#include "yyjson.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace xi {

class DocRegistry {
public:
    static DocRegistry& instance() {
        static DocRegistry r;
        return r;
    }

    // Bump the refcount for `doc`, creating the entry at 1 if it is not yet
    // registered (the first side to share an owned doc registers it). Named
    // `addref` to match ImagePool::addref — one refcount verb for both handle
    // domains (the ABI field stays `doc_retain`, frozen; this is the C++ method).
    void addref(yyjson_mut_doc* doc) {
        if (!doc) return;
        Shard& sh = shard_for_(doc);
        std::lock_guard<std::mutex> lk(sh.mu);
        ++sh.rc[doc];
    }

    // Drop one ref; when the count reaches zero, free the doc. The actual
    // yyjson_mut_doc_free runs OUTSIDE the lock — it returns chunks to the
    // thread-local DocChunkPool and there's no reason to hold a shard mutex
    // across it.
    void release(yyjson_mut_doc* doc) {
        if (!doc) return;
        Shard& sh = shard_for_(doc);
        bool free_it = false;
        {
            std::lock_guard<std::mutex> lk(sh.mu);
            auto it = sh.rc.find(doc);
            if (it == sh.rc.end()) return;   // not managed — ignore (defensive)
            if (--it->second <= 0) {
                sh.rc.erase(it);
                free_it = true;
            }
        }
        if (free_it) yyjson_mut_doc_free(doc);
    }

    // Diagnostics / tests: current refcount (0 if not registered). Does not
    // dereference `doc` — safe to call after the doc was freed.
    int refcount(yyjson_mut_doc* doc) const {
        if (!doc) return 0;
        const Shard& sh = shard_for_(doc);
        std::lock_guard<std::mutex> lk(sh.mu);
        auto it = sh.rc.find(doc);
        return it == sh.rc.end() ? 0 : it->second;
    }

    // Number of distinct docs currently registered across all shards (leak
    // watchdog for tests).
    size_t live_count() const {
        size_t n = 0;
        for (const auto& sh : shards_) {
            std::lock_guard<std::mutex> lk(sh.mu);
            n += sh.rc.size();
        }
        return n;
    }

private:
    static constexpr size_t kShards = 16;   // power of two

    struct Shard {
        mutable std::mutex mu;
        std::unordered_map<yyjson_mut_doc*, int> rc;
    };
    Shard shards_[kShards];

    Shard& shard_for_(yyjson_mut_doc* doc) {
        // Drop the low (alignment) bits, then mask to a shard index.
        uintptr_t p = reinterpret_cast<uintptr_t>(doc) >> 4;
        return shards_[p & (kShards - 1)];
    }
    const Shard& shard_for_(yyjson_mut_doc* doc) const {
        uintptr_t p = reinterpret_cast<uintptr_t>(doc) >> 4;
        return shards_[p & (kShards - 1)];
    }
};

// Move-only RAII owner of ONE DocRegistry ref on a yyjson_mut_doc*. Replaces the
// hand-rolled "whoever holds this doc owns one ref and MUST DocRegistry::release()
// it at every drop/consume site" discipline that TriggerEvent::meta_doc used to
// carry as a raw pointer (the same class of manual-per-path-release bug the
// TriggerEventReleaser / CurrentTriggerScope guards exist to kill). Semantics
// mirror that protocol EXACTLY so the retain/release COUNT is unchanged:
//   - adopt(doc): take ownership of an ALREADY-HELD ref — NO retain. The emit /
//     stage / cmd:run paths hand over a ref that share_out reserved or that a
//     retain-at-create just registered, so adopting must not add a ref.
//   - reset() / dtor: DocRegistry::release() the held ref exactly once, then null
//     — so a dtor that runs after an explicit reset() is a no-op (no double
//     release). This is what lets release_trigger_event_() keep releasing at the
//     drop site while the struct still gains an implicit destructor as a backstop.
//   - move: transfers the ref; the moved-from DocRef is null (releases nothing) —
//     so `sink(std::move(ev))` moves the doc ref out without a double free.
//   - copy: deleted — a ref cannot be duplicated without an explicit retain.
class DocRef {
public:
    DocRef() noexcept = default;

    // Adopt an existing ref (no retain). NAMED so "no retain here" is unmistakable
    // at every call site that transfers a reserved/registered ref into the event.
    static DocRef adopt(yyjson_mut_doc* doc) noexcept { return DocRef(doc); }

    DocRef(DocRef&& o) noexcept : doc_(o.doc_) { o.doc_ = nullptr; }
    DocRef& operator=(DocRef&& o) noexcept {
        if (this != &o) { reset(); doc_ = o.doc_; o.doc_ = nullptr; }
        return *this;
    }
    DocRef(const DocRef&)            = delete;
    DocRef& operator=(const DocRef&) = delete;
    ~DocRef() { reset(); }

    // Release the held ref (if any) and null. Effectively noexcept — the registry
    // release only locks a shard + frees the doc (returns chunks to a pool); it
    // never throws in practice (same assumption the drop guards already make).
    void reset() noexcept {
        if (doc_) { DocRegistry::instance().release(doc_); doc_ = nullptr; }
    }

    // Borrowed read of the underlying pointer — does NOT change the refcount.
    yyjson_mut_doc* get() const noexcept { return doc_; }
    explicit operator bool() const noexcept { return doc_ != nullptr; }

private:
    explicit DocRef(yyjson_mut_doc* doc) noexcept : doc_(doc) {}
    yyjson_mut_doc* doc_ = nullptr;
};

} // namespace xi
