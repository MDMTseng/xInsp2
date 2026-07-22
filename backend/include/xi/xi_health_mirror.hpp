#pragma once
//
// xi_health_mirror.hpp — the FE health-mirror file write, factored out so it is
// (a) concurrency-safe and (b) unit-testable without the Engine.
//
// The mirror is the tiny top-level-health file the FE supervisor polls (it can't
// call get_health without stealing the single WS client slot; see
// docs/internals/fe-be.md). It is (re)written from HealthRegistry's
// health_changed notifier — which fires with the registry's mu_ RELEASED
// (emit_ unlocks before invoking the notifier, xi_health.hpp). So two workers
// faulting DIFFERENT instances concurrently can both land in this writer at once.
//
// The write MUST therefore serialize across threads itself: a function-static
// mutex fully orders the writers. This is the load-bearing guarantee — writes are
// lifecycle-rate (only on health_changed), so contention cost is irrelevant.
//
// The actual bytes go out via xi::atomic_write (tmp + FlushFileBuffers + rename):
// durable, and it leaves the canonical file INTACT on any failure — no in-place
// truncation. NOTE: atomic_write uses a fixed "<path>.tmp", so it is NOT
// concurrency-safe on its own; the mutex here is what makes concurrent writers
// safe. Best-effort: a failed mirror must never affect the BE.
//
#include <mutex>
#include <string>

#include "xi_atomic_io.hpp"   // xi::atomic_write

namespace xi {

// Write `json` to the mirror at `path`, fully serialized against all other
// callers. Empty path = mirror disabled (a bare/manually-run BE). Returns the
// atomic_write result (false = write failed, canonical file left untouched).
inline bool write_health_mirror_file(const std::string& path,
                                     const std::string& json) {
    if (path.empty()) return true;
    // Function-static: one mutex shared across every caller (this is an inline
    // function, so its local static is a single object across all TUs). Serializes
    // the whole write so two concurrent writers can never share the "<path>.tmp"
    // scratch file or tear the canonical file.
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    return xi::atomic_write(path, json);
}

}  // namespace xi
