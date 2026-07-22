#pragma once
//
// xi_cap_abi.hpp — the HOST side of the capability plane (docs/new_gen/14,
// polaris2 pilot): lib plugins register named, pack-door-shaped capabilities;
// consumers call them by NAME through the host forwarding funnel. The plane
// costs ZERO xi_host_api slots — both directions ride get_interface:
//
//   provider: get_interface("xi.cap.provider", 1) -> xi_cap_provider_v1
//   consumer: get_interface("xi.cap", 1)          -> xi_cap_v1
//
// This header is the sibling of xi_pack_abi.hpp and follows its shape exactly:
//
//   * CapRegistry — the NAME-ONLY registry: capability name -> {handler, self,
//     owning instance}. Names carry no version; semantic versioning rides
//     INSIDE the request pack as the reserved "$v" i64 entry (the provider
//     dispatches internally and answers an unsupported "$v" with a sealed
//     $fault pack), and "$probe": true asks for the supported versions with no
//     work done. Registration is attributed to the CALLING instance via the
//     thread's ImagePool owner context (the same context every plugin entry
//     point already runs under), and is legal ONLY from lifecycle code —
//     create / set_def / prepare / commit / exchange / destroy — never from a
//     data-plane door or a capability handler (xi_cap_guard.hpp's depth mark).
//
//   * cap_call_funnel — the forwarding funnel, mirroring use_pack_process_cb's
//     discipline (backend/src/service_sinks.cpp) in NEW code: quarantine
//     fail-fast (-3), pending-reinit applied before entry, SEH wrap with the
//     fault CHARGED TO THE LIB INSTANCE (its on_fault policy: reuse / reinit /
//     refuse->quarantine) (-2), unknown name (-1), unusable entry (-4), and
//     doc 14's one new invariant: per-thread acyclicity — forwarding into an
//     instance already being called on THIS thread is refused with -5
//     (XI_CAP_EREENTRY) before any lock or plugin code is reached, in BOTH
//     directions (a plugin calling back into itself through a capability, and
//     a capability handler calling back up its own call chain).
//
//   * install_cap_plane() — publishes the two vtables + the owner sweeper into
//     ImagePool's slots (the same layering bridge as install_pack_abi), so
//     host->get_interface answers both ids. Idempotent; call next to
//     install_pack_abi (default_host_api / tests).
//
// NO CallScope around the handler: providers CONTRACT to be thread-safe
// (funnel calls arrive concurrently from multiple dispatch threads) — an
// ABI-contract requirement, same as the pack vtable. The handler runs under
// OwnerGuard(provider) so its pool/pack allocations are attributed to the lib
// instance exactly like every other entry point.
//

#include "xi_abi.h"           // xi_cap_v1 / xi_cap_provider_v1 / result codes
#include "xi_cap_guard.hpp"   // reentrancy stack + data-plane depth (leaf)
#include "xi_cabi_adapter.hpp"// CAbiInstanceAdapter (quarantine/on_fault surface)
#include "xi_fault_policy.hpp"// OnFault + kReinitEscalateAfter
#include "xi_health.hpp"      // health overlay marks (quarantine/recovered)
#include "xi_image_pool.hpp"  // owner context + the publish slots
#include "xi_instance.hpp"    // InstanceRegistry (owner -> live adapter)
#include "xi_seh.hpp"         // seh_exception + recover_seh_stack_or_die
#include "xi_clock.hpp"       // R2: mono_us() for cap-call latency metering
#include "xi_json_escape.hpp" // the ONE JSON string-escape primitive (std-only leaf)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace xi {

// ===================================================================
// CapRegistry — capability name -> providing instance's handler.
// ===================================================================
class CapRegistry {
public:
    struct Entry {
        xi_cap_handler_fn handler = nullptr;
        void*             self    = nullptr;
        ImagePoolOwnerId  owner   = 0;      // the providing instance's identity
    };

    // INTENTIONALLY LEAKED (mirrors ImagePool/PackRegistry::instance):
    // process-lifetime state, never destroyed — so late teardown sweeps (a
    // static-destruction adapter dtor) can touch the registry unconditionally,
    // with no destroyed-singleton window and no registry-liveness guard flag.
    // The OS reclaims the memory at exit.
    static CapRegistry& instance() {
        static CapRegistry* r = new CapRegistry();
        return *r;
    }

    // Caller has already validated args + lifecycle context (f_cap_register).
    // Same-owner re-registration OVERWRITES (the reinit path: the fresh factory
    // re-registers over its predecessor). A name held by ANOTHER live instance
    // is still REFUSED to the caller (XI_CAP_REG_ETAKEN — provider identity is
    // configuration, not a race to the slot), BUT the losing registration is now
    // REMEMBERED as a shadow (B1 fix): if the current holder is later removed,
    // the most-recent live shadow is PROMOTED so the capability keeps resolving.
    // Before this, a 2nd project instance of a provider plugin got ETAKEN and was
    // silently live-but-unregistered; removing the 1st instance then swept the
    // name, leaving the capability permanently GONE with a live provider.
    int32_t register_capability(const char* name, xi_cap_handler_fn handler,
                                void* self, ImagePoolOwnerId owner) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(name);
        if (it == entries_.end()) {
            entries_[name] = Entry{handler, self, owner};
            return XI_CAP_REG_OK;
        }
        if (it->second.owner == owner) {   // same owner: overwrite (reinit path)
            it->second = Entry{handler, self, owner};
            return XI_CAP_REG_OK;
        }
        // Held by another owner: shadow it (dedup per owner) and report ETAKEN.
        auto& st = shadows_[name];
        for (auto& e : st)
            if (e.owner == owner) { e = Entry{handler, self, owner}; return XI_CAP_REG_ETAKEN; }
        st.push_back(Entry{handler, self, owner});
        return XI_CAP_REG_ETAKEN;
    }

    // Unregister by name, gated on the calling instance's identity (only the
    // owner may drop its registration; `self` is not trusted as identity). If
    // the owner held the ACTIVE name, promote a shadow into its place (B1).
    int32_t unregister_capability(const char* name, ImagePoolOwnerId owner) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(name);
        if (it != entries_.end() && it->second.owner == owner) {
            promote_or_erase_(it);
            return XI_CAP_REG_OK;
        }
        // Not the active holder — maybe a shadow the owner still has queued.
        if (drop_shadow_(name, owner)) return XI_CAP_REG_OK;
        return XI_CAP_REG_EINVAL;
    }

    bool lookup(const char* name, Entry* out) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(name);
        if (it == entries_.end()) return false;
        if (out) *out = it->second;
        return true;
    }

    // The owner sweep — the registration analogue of PackRegistry::
    // release_all_for. Drops every capability the dying (or about-to-be-
    // rebuilt) instance still provides — as the ACTIVE holder (promoting a
    // shadow into its place) OR as a queued shadow; returns the count for the
    // teardown diagnostic.
    int unregister_all_for(ImagePoolOwnerId owner) {
        if (owner == 0) return 0;
        std::lock_guard<std::mutex> lk(mu_);
        int n = 0;
        // Drop this owner from every shadow queue first (a losing registration
        // this instance never got to activate).
        for (auto sit = shadows_.begin(); sit != shadows_.end();) {
            auto& st = sit->second;
            for (auto e = st.begin(); e != st.end();) {
                if (e->owner == owner) { e = st.erase(e); ++n; } else ++e;
            }
            if (st.empty()) sit = shadows_.erase(sit); else ++sit;
        }
        // Then any ACTIVE name this owner holds: promote a shadow or erase it.
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.owner == owner) { it = promote_or_erase_(it); ++n; }
            else ++it;
        }
        return n;
    }

    size_t size() {
        std::lock_guard<std::mutex> lk(mu_);
        return entries_.size();
    }

private:
    // Replace the active entry at `it` with the most-recent shadow (if any) or
    // erase it. Returns the iterator to the next active entry (erase-safe).
    std::unordered_map<std::string, Entry>::iterator
    promote_or_erase_(std::unordered_map<std::string, Entry>::iterator it) {
        auto sit = shadows_.find(it->first);
        if (sit != shadows_.end() && !sit->second.empty()) {
            it->second = sit->second.back();
            sit->second.pop_back();
            if (sit->second.empty()) shadows_.erase(sit);
            return std::next(it);   // promoted in place — advance past it
        }
        return entries_.erase(it);
    }

    bool drop_shadow_(const std::string& name, ImagePoolOwnerId owner) {
        auto sit = shadows_.find(name);
        if (sit == shadows_.end()) return false;
        auto& st = sit->second;
        for (auto e = st.begin(); e != st.end(); ++e)
            if (e->owner == owner) {
                st.erase(e);
                if (st.empty()) shadows_.erase(sit);
                return true;
            }
        return false;
    }

    std::mutex mu_;
    std::unordered_map<std::string, Entry> entries_;
    // Shadowed (ETAKEN) registrations, most-recent last, promoted when the
    // active holder for the name is unregistered/swept (B1 ref-count).
    std::unordered_map<std::string, std::vector<Entry>> shadows_;
};

namespace cap_abi_detail {

// ---- optional service enrichment hooks -------------------------------------
// The funnel's fault mechanics (on_fault policy + health overlay) are complete
// header-side; the SERVICE additionally keeps crash-loop counts + a recent-
// errors ring + crash-culprit forensics (note_instance_crash_ / stamp_culprit_,
// service-layer state this header cannot reach). It installs these hooks at
// startup; tests and headless hosts run fine without them.
using CapFaultNoteFn = void (*)(const char* instance, const char* why);
using CapPrecallFn   = void (*)(const char* instance, const char* plugin);
inline std::atomic<CapFaultNoteFn>& fault_note_slot() {
    static std::atomic<CapFaultNoteFn> s{nullptr};
    return s;
}
inline std::atomic<CapPrecallFn>& precall_slot() {
    static std::atomic<CapPrecallFn> s{nullptr};
    return s;
}

// ---- item-14 fault mechanics, capability-funnel edition ---------------------
// Same POLICY semantics as service_sinks.cpp's quarantine_instance_ /
// apply_on_fault_policy_ / apply_pending_reinit_, factored as NEW functions
// (this funnel is a new code path; the service statics stay untouched).
inline void quarantine_(CAbiInstanceAdapter* a) {
    if (a->quarantined()) return;
    a->set_quarantined(true);
    health().mark_instance_fault(a->name(), CompHealth::Failed, kReasonQuarantined);
}
inline void apply_on_fault_(CAbiInstanceAdapter* a) {
    switch (a->on_fault()) {
        case OnFault::Reuse:  break;
        case OnFault::Reinit: a->request_reinit(); break;
        case OnFault::Refuse: quarantine_(a); break;
    }
}
inline void apply_pending_reinit_(CAbiInstanceAdapter* a) {
    // H5 (cap-funnel sibling of service_sinks.cpp): atomically consume the pending
    // flag so two concurrent faulters can't both rebuild (double-rebuild + spurious
    // quarantine). Only the exchange winner reinit()s + accounts.
    if (!a->consume_reinit_pending()) return;
    if (a->reinit()) {
        a->reset_reinit_fails();
        health().clear_instance_degraded(a->name());
    } else if (a->note_reinit_fail() >= kReinitEscalateAfter) {
        quarantine_(a);
    }
}

// Resolve the providing instance by its owner identity. Registration happens
// in the plugin FACTORY (before the adapter exists in the registry), so a call
// racing instance creation resolves nothing and fails soft (-4); once the
// instance is in service the scan finds it. Linear over live instances —
// bounded small, and the plane is sized for HEAVY calls (doc 14 sizing
// doctrine), so the scan is noise. The returned shared_ptr pins the instance
// for the duration of the call.
inline std::shared_ptr<InstanceBase> resolve_provider_(ImagePoolOwnerId owner,
                                                       CAbiInstanceAdapter** out) {
    *out = nullptr;
    for (auto& p : InstanceRegistry::instance().list()) {
        auto* a = dynamic_cast<CAbiInstanceAdapter*>(p.get());
        if (a && a->owner_id() == owner) { *out = a; return p; }
    }
    return nullptr;
}

// ---- the forwarding funnel --------------------------------------------------
// R2 (doc 14 "call-count/latency observability for free"): per-capability call
// metering for the funnel. A HEAVY-call plane by doctrine, so a plain mutex+map
// recorded once per call is negligible. Exposed via cmd:metrics "capabilities".
class CapMetrics {
public:
    static CapMetrics& instance() { static CapMetrics m; return m; }

    void record(const char* name, int32_t rc, uint64_t us) {
        if (!name || !*name) return;
        std::lock_guard<std::mutex> lk(mu_);
        Stat& s = stats_[name];
        s.calls++;
        if (rc != XI_CAP_OK) s.errors++;
        s.total_us += us;
        if (us > s.max_us) s.max_us = us;
    }

    // A JSON object: { "<cap name>": {calls,errors,total_us,max_us}, ... }.
    void snapshot_json(std::string& out) const {
        std::lock_guard<std::mutex> lk(mu_);
        out += "{";
        bool first = true;
        for (const auto& [name, s] : stats_) {
            if (!first) out += ",";
            first = false;
            json_escape_into(out, name);   // the ONE escape primitive (quotes included)
            out += ":{\"calls\":"          + std::to_string(s.calls)
                 + ",\"errors\":"      + std::to_string(s.errors)
                 + ",\"total_us\":"    + std::to_string(s.total_us)
                 + ",\"max_us\":"      + std::to_string(s.max_us) + "}";
        }
        out += "}";
    }

    void reset() { std::lock_guard<std::mutex> lk(mu_); stats_.clear(); }

private:
    struct Stat { uint64_t calls = 0, errors = 0, total_us = 0, max_us = 0; };
    mutable std::mutex           mu_;
    std::map<std::string, Stat>  stats_;   // sorted → stable snapshot order
};

// The metered entry point wraps the implementation: time the whole call (lookup +
// resolve + handler) and record the final rc against the capability name.
inline int32_t f_cap_call_impl(const char* name, xi_pack_handle in, xi_pack_handle* out) {
    if (out) *out = XI_PACK_NULL;
    if (!name || !*name || !out || in == XI_PACK_NULL) return XI_CAP_EUNKNOWN;
    CapRegistry::Entry e;
    if (!CapRegistry::instance().lookup(name, &e)) return XI_CAP_EUNKNOWN;

    CAbiInstanceAdapter* adapter = nullptr;
    auto pin = resolve_provider_(e.owner, &adapter);
    if (!adapter || !e.handler) return XI_CAP_ESHAPE;

    // item-14 gates, same order as use_pack_process_cb: quarantine fail-fast,
    // then pending reinit applied off-frame BEFORE entering plugin code.
    if (adapter->quarantined()) return XI_CAP_EQUARANTINED;
    if (adapter->reinit_pending()) {
        apply_pending_reinit_(adapter);
        if (adapter->quarantined()) return XI_CAP_EQUARANTINED;
        // The rebuild swept this owner's registrations and the fresh factory
        // re-registered — re-resolve so the handler/self are the LIVE pair.
        if (!CapRegistry::instance().lookup(name, &e)) return XI_CAP_EUNKNOWN;
        if (e.owner != adapter->owner_id() || !e.handler) return XI_CAP_ESHAPE;
    }

    // Doc 14's reentrancy ruling, BOTH directions: the consumer calling a
    // capability its own instance provides (target == this thread's current
    // owner), and a handler calling back up its chain (target already on this
    // thread's funnel stack). Refused before any lock/plugin code — the
    // deadlock (a CallScope slot this thread may hold) dies at the door.
    ImagePoolOwnerId cur = ImagePool::current_owner();
    if (e.owner == cur || cap_stack_contains(e.owner)) return XI_CAP_EREENTRY;

    if (auto pre = precall_slot().load(std::memory_order_acquire))
        pre(adapter->name().c_str(), adapter->plugin_name().c_str());

    // Frames: consumer (if any) + provider on the per-thread stack; the
    // handler is data plane (no registration inside); pool/pack allocations
    // attributed to the LIB instance.
    CapStackFrame consumer_frame((uint64_t)cur);
    CapStackFrame provider_frame((uint64_t)e.owner);
    DataPlaneMark dp;
    ImagePool::OwnerGuard og(e.owner);
    // A1/A2 (redteam doc 21): pin inst_ against a concurrent reinit for the whole
    // handler run. reinit destroys the OLD inst_ under the EXCLUSIVE side of this
    // gate; hold the SHARED side here and RE-RESOLVE the live handler/self UNDER
    // it, so we never call a handler whose inst_ a reinit is about to (or already
    // did) free. If reinit swept+rebuilt in the window, the fresh registration is
    // visible under the lock (fresh self); the mid-sweep gap reads as EUNKNOWN.
    std::shared_lock<std::shared_mutex> reinit_lk(adapter->cap_reinit_gate());
    if (!CapRegistry::instance().lookup(name, &e)) return XI_CAP_EUNKNOWN;
    if (e.owner != adapter->owner_id() || !e.handler) return XI_CAP_ESHAPE;
    try {
        *out = e.handler(e.self, in);
        // Ownership handoff (xi_pack_abi.hpp): the handler ran under
        // OwnerGuard(e.owner), so its sealed OUTPUT pack carries the PROVIDER's
        // creator tag — but the seal ref is handed to the CALLER, who owns it.
        // Left tagged, the provider's teardown sweep (adapter dtor →
        // sweep_packs_for → release_all_for) would reclaim the handed-off ref
        // and free the consumer's live pack (wrong answer / data loss). Clear
        // the creator tag while the provider pin is still held — rc unchanged
        // (PackRegistry::untag). Slot bridge (no-op until install_pack_abi —
        // same layering as sweep_packs_for).
        // A NULL return is the handler's declared HARD-INTERNAL-FAILURE sentinel
        // (xi_abi.h) — surface it as an error, never XI_CAP_OK, so a consumer
        // that trusts "OK ⇒ non-null *out" is not silently handed a NULL. (A
        // CONTRACT failure is a normal sealed $fault pack, not NULL.)
        if (*out == XI_PACK_NULL) return XI_CAP_EINTERNAL;
        ImagePool::untag_pack_ref(*out, e.owner);
        return XI_CAP_OK;
    } catch (const seh_exception& ex) {
        std::fprintf(stderr, "[xinsp2] capability '%s' handler crashed: 0x%08X (%s)\n",
                     name, ex.code, ex.what());
        char why[96];
        std::snprintf(why, sizeof(why), "capability handler crashed: 0x%08X", ex.code);
        if (auto fn = fault_note_slot().load(std::memory_order_acquire))
            fn(adapter->name().c_str(), why);
        apply_on_fault_(adapter);
        // Same rationale as use_pack_process_cb: this boundary swallows the
        // fault and the caller continues on this thread — restore the guard
        // page (or hard-exit for respawn) before returning.
        recover_seh_stack_or_die(ex.code, "capability handler");
        *out = XI_PACK_NULL;   // a torn result is never handed out
        return XI_CAP_ECRASHED;
    } catch (...) {
        std::fprintf(stderr, "[xinsp2] capability '%s' handler threw exception\n", name);
        if (auto fn = fault_note_slot().load(std::memory_order_acquire))
            fn(adapter->name().c_str(), "capability handler threw an exception");
        apply_on_fault_(adapter);
        *out = XI_PACK_NULL;
        return XI_CAP_ECRASHED;
    }
}

// R2: metered funnel entry — times the whole call and records it per capability.
inline int32_t f_cap_call(const char* name, xi_pack_handle in, xi_pack_handle* out) {
    uint64_t t0 = (uint64_t)xi::mono_us();
    int32_t rc = f_cap_call_impl(name, in, out);
    CapMetrics::instance().record(name, rc, (uint64_t)xi::mono_us() - t0);
    return rc;
}

inline int32_t f_cap_available(const char* name) {
    if (!name || !*name) return 0;
    return CapRegistry::instance().lookup(name, nullptr) ? 1 : 0;
}

// ---- registration trampolines -----------------------------------------------
inline int32_t f_cap_register(const char* name, xi_cap_handler_fn handler,
                              void* self) {
    if (!name || !*name || !handler) return XI_CAP_REG_EINVAL;
    // Lifecycle enforcement: the registering thread must carry an owner
    // context (every plugin entry point does — factory included) and must NOT
    // be inside a data-plane door or a capability handler.
    ImagePoolOwnerId owner = ImagePool::current_owner();
    if (owner == 0 || cap_data_plane_depth() > 0) return XI_CAP_REG_ECONTEXT;
    return CapRegistry::instance().register_capability(name, handler, self, owner);
}
inline int32_t f_cap_unregister(const char* name, void* /*self*/) {
    if (!name || !*name) return XI_CAP_REG_EINVAL;
    ImagePoolOwnerId owner = ImagePool::current_owner();
    // Symmetric with f_cap_register: registry mutation is legal only from
    // lifecycle code, never from a data-plane door or a capability handler (a
    // provider unregistering mid-handler could trigger a surprise shadow-promotion
    // of another instance while this handler still runs). Guard depth here too.
    if (owner == 0 || cap_data_plane_depth() > 0) return XI_CAP_REG_ECONTEXT;
    return CapRegistry::instance().unregister_capability(name, owner);
}

// The owner-sweep trampoline published into ImagePool::cap_sweep_slot, so the
// teardown paths that already sweep leaked image handles + pack refs (adapter
// dtor) and the reinit rebuild drop this owner's registrations through the
// same layering bridge. Safe even during static destruction: the registry
// singleton is intentionally leaked.
inline int f_cap_sweep_for(ImagePoolOwnerId owner) {
    return CapRegistry::instance().unregister_all_for(owner);
}

} // namespace cap_abi_detail

// Install a service-side fault-bookkeeping hook (crash-loop count / recent-
// errors ring — note_instance_crash_'s job) and an optional pre-call culprit
// stamp. Null = header-side mechanics only (tests, headless hosts).
inline void set_cap_fault_note(cap_abi_detail::CapFaultNoteFn fn) {
    cap_abi_detail::fault_note_slot().store(fn, std::memory_order_release);
}
inline void set_cap_precall(cap_abi_detail::CapPrecallFn fn) {
    cap_abi_detail::precall_slot().store(fn, std::memory_order_release);
}

// The process-stable vtables. Meyers singletons — a plugin caches the pointer
// once, exactly like pack_v1_iface.
inline const xi_cap_v1* cap_v1_iface() {
    static const xi_cap_v1 iface = {
        &cap_abi_detail::f_cap_call,
        &cap_abi_detail::f_cap_available,
    };
    return &iface;
}
inline const xi_cap_provider_v1* cap_provider_v1_iface() {
    static const xi_cap_provider_v1 iface = {
        &cap_abi_detail::f_cap_register,
        &cap_abi_detail::f_cap_unregister,
    };
    return &iface;
}

// Publish the capability plane so host->get_interface answers "xi.cap"@1 and
// "xi.cap.provider"@1, and wire the owner sweeper into the teardown bridge.
// Idempotent; call once next to install_pack_abi (default_host_api / tests).
inline void install_cap_plane() {
    ImagePool::publish_cap_iface(cap_v1_iface());
    ImagePool::publish_cap_provider_iface(cap_provider_v1_iface());
    ImagePool::publish_cap_sweeper(&cap_abi_detail::f_cap_sweep_for);
}

} // namespace xi
