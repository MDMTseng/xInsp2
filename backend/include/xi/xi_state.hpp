#pragma once
//
// xi_state.hpp — persistent cross-frame state for inspection scripts.
//
// xi::state is a global Record that survives across frames AND across
// hot-reloads. The backend serializes it before unloading a script DLL
// and restores it after loading the new one.
//
// Usage in a script:
//
//   void xi_inspect_entry(int frame) {
//       int count = xi::state["frame_count"].as_int(0);
//       double yield = xi::state["yield"].as_double(1.0);
//
//       // ... inspection logic ...
//
//       xi::state.set("frame_count", count + 1);
//       xi::state.set("yield", new_yield);
//       xi::state.set("last_result", xi::Record()
//           .set("pass", true)
//           .set("score", 0.95));
//   }
//
// State persists:
//   ✓ across frames (normal execution)
//   ✓ across hot-reloads (save → unload → load → restore)
//   ✓ across save/load project
//   ✗ across backend restarts (unless project is saved)
//

#include "xi_record.hpp"

namespace xi {

// The global persistent state. Lives in each DLL's address space.
// The backend reads/writes it via thunks before/after each reload.
inline Record& state() {
    static Record s;
    return s;
}

// Convenience: xi::state["key"] syntax via a macro-free global reference.
// Users write xi::state()["key"].as_int() or just use the function directly.

} // namespace xi
