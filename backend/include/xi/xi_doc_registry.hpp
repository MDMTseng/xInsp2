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
// Backs host_api.doc_retain / doc_release (xi_abi.h v4). This is a LOW-FREQUENCY
// path — entered only when a doc is genuinely shared across the boundary, not
// on every node — so a single mutex over a small map is enough; it does not
// need ImagePool's lock-free slot array. The hot in-process path (rc==1, no
// cross-side holder) never touches the registry.
//
// TODO(linux): std::mutex + unordered_map only — already cross-platform.
//

#include "yyjson.h"

#include <cstddef>
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
    // registered (the first side to share an owned doc registers it).
    void retain(yyjson_mut_doc* doc) {
        if (!doc) return;
        std::lock_guard<std::mutex> lk(mu_);
        ++rc_[doc];
    }

    // Drop one ref; when the count reaches zero, free the doc. The actual
    // yyjson_mut_doc_free runs OUTSIDE the lock — it returns chunks to the
    // thread-local DocChunkPool and there's no reason to hold the registry
    // mutex across it.
    void release(yyjson_mut_doc* doc) {
        if (!doc) return;
        bool free_it = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = rc_.find(doc);
            if (it == rc_.end()) return;   // not managed — ignore (defensive)
            if (--it->second <= 0) {
                rc_.erase(it);
                free_it = true;
            }
        }
        if (free_it) yyjson_mut_doc_free(doc);
    }

    // Diagnostics / tests: current refcount (0 if not registered). Does not
    // dereference `doc` — safe to call after the doc was freed.
    int refcount(yyjson_mut_doc* doc) const {
        if (!doc) return 0;
        std::lock_guard<std::mutex> lk(mu_);
        auto it = rc_.find(doc);
        return it == rc_.end() ? 0 : it->second;
    }

    // Number of distinct docs currently registered (leak watchdog for tests).
    size_t live_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return rc_.size();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<yyjson_mut_doc*, int> rc_;
};

} // namespace xi
