#pragma once
//
// xi_cap_guard.hpp — the thread-local state behind the capability plane's two
// gates (docs/new_gen/14, polaris2 pilot). A LEAF header (no xi includes) so
// BOTH sides can see it without a layering knot:
//
//   * xi_cabi_adapter.hpp marks its data-plane doors (process / pack door)
//     with DataPlaneMark, so registration can be refused there;
//   * xi_cap_abi.hpp (the host funnel) maintains the per-thread CALL STACK the
//     reentrancy ruling is enforced against, and consults both.
//
// 1. THE REENTRANCY STACK (doc 14's one new invariant). Owner ids (the
//    ImagePool owner identity every adapter carries — uint32_t, passed here as
//    uint64_t to stay leaf) of instances currently being called through host
//    funnels ON THIS THREAD. The capability funnel pushes the CONSUMER (the
//    thread's current owner at call time) and the PROVIDER around each handler
//    invocation; forwarding into an instance already on the stack is refused
//    with XI_CAP_EREENTRY (-5) BEFORE any lock or plugin code is reached, so
//    deadlock (the CallScope slot this thread may hold) and recursion die at
//    the door. Cross-THREAD concurrency never appears on this stack and is
//    correctly not flagged — providers contract to be thread-safe.
//
// 2. THE DATA-PLANE DEPTH. > 0 while this thread is inside a data-plane door
//    (a plugin's process()/pack door) or a capability handler. Capability
//    REGISTRATION is a lifecycle act (create / set_def / prepare / commit /
//    exchange / destroy); registering from the data plane is refused with
//    XI_CAP_REG_ECONTEXT (-2) so the registry can only change under the
//    lifecycle discipline the owner sweep is built around.
//

#include <cstdint>
#include <vector>

namespace xi {

// Per-thread stack of owner ids currently inside a host-funnel call chain.
inline std::vector<uint64_t>& cap_thread_stack() {
    thread_local std::vector<uint64_t> s;
    return s;
}

inline bool cap_stack_contains(uint64_t owner) {
    if (owner == 0) return false;
    for (uint64_t v : cap_thread_stack())
        if (v == owner) return true;
    return false;
}

// RAII frame: pushes an owner id (0 = no-op) on construction, pops on exit.
struct CapStackFrame {
    bool engaged_ = false;
    explicit CapStackFrame(uint64_t owner) {
        if (owner != 0) { cap_thread_stack().push_back(owner); engaged_ = true; }
    }
    ~CapStackFrame() { if (engaged_) cap_thread_stack().pop_back(); }
    CapStackFrame(const CapStackFrame&) = delete;
    CapStackFrame& operator=(const CapStackFrame&) = delete;
};

// Per-thread "inside a data-plane door / capability handler" depth.
inline int& cap_data_plane_depth() {
    thread_local int d = 0;
    return d;
}

// RAII data-plane marker for the adapter's doors + the capability funnel.
struct DataPlaneMark {
    DataPlaneMark()  { ++cap_data_plane_depth(); }
    ~DataPlaneMark() { --cap_data_plane_depth(); }
    DataPlaneMark(const DataPlaneMark&) = delete;
    DataPlaneMark& operator=(const DataPlaneMark&) = delete;
};

} // namespace xi
