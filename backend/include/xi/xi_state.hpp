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

} // namespace xi
