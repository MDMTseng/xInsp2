#pragma once
//
// xi_state.hpp — persistent cross-frame state for inspection scripts.
//
// Thread safety: state() returns a reference guarded by state_mutex().
// Callers from xi::async tasks must lock before read/write:
//
//   {
//       std::lock_guard<std::mutex> lk(xi::state_mutex());
//       xi::state().set("count", count + 1);
//   }
//
// Single-threaded scripts (no xi::async) can ignore the mutex.
//

#include "xi_record.hpp"

#include <atomic>
#include <mutex>

namespace xi {

inline Record& state() {
    static Record s;
    return s;
}

inline std::mutex& state_mutex() {
    static std::mutex mu;
    return mu;
}

// Schema version of `xi::state()`'s shape. Bump when you rename a
// key, change a type, etc — the host drops the persisted state on a
// version mismatch (event:state_dropped) instead of letting set_state
// default-fill into a different shape.
//
// Register from user-script code at static-init time:
//
//   static int _sv = (xi::set_state_schema_version(2), 0);
//
// or via the convenience macro:
//
//   XI_STATE_SCHEMA(2);
//
// The earlier `#define XI_STATE_SCHEMA_VERSION 2; #include <xi/xi.hpp>`
// pattern didn't work because xi_script_support.hpp is force-included
// (`cl.exe /FI`) BEFORE the user TU is parsed, so the user's #define
// arrived too late. The runtime setter side-steps that.
inline std::atomic<int>& state_schema_version_ref() {
    static std::atomic<int> v{0};
    return v;
}
inline int  state_schema_version()        { return state_schema_version_ref().load(std::memory_order_relaxed); }
inline void set_state_schema_version(int v) { state_schema_version_ref().store(v, std::memory_order_relaxed); }

} // namespace xi

// Convenience macro: declare a static initialiser that registers the
// version once at DLL load. Place at file scope.
#define XI_STATE_SCHEMA(N) \
    namespace { static int _xi_state_schema_init_##N = (xi::set_state_schema_version(N), 0); }
