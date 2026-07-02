#pragma once
//
// xi_fault_policy.hpp — the per-instance POST-FAULT policy vocabulary
// (adoption-map item 14). Design: docs/new_gen/04-health-contract.md
// (§ "Quarantine policy"), building on the health contract's runtime-fault
// overlay (xi_health.hpp — the quarantine SEED this policy acts on).
//
// When a plugin instance's process()/exchange() faults and the SEH boundary
// catches it (service_sinks.cpp use_process_inline_), the instance is — by
// default — logged and reused. That is correct for a STATELESS operator, but a
// STATEFUL plugin caught mid-mutation may carry corrupt-but-not-fatal state into
// the next frame (concurrency review 08, finding 5). `on_fault` lets a plugin (or
// a per-instance override) declare what should happen instead:
//
//   reuse  — today's behavior: the fault is logged + the instance is marked
//            runtime-`degraded` in the health overlay, but it stays in service.
//            Correct for stateless operators. THE DEFAULT (pre-1.0 compatible —
//            nothing changes unless a plugin/instance declares otherwise).
//   reinit — after a caught fault the plugin instance is TORN DOWN and re-created
//            + re-prepared from its last committed config before its next use, so
//            a corrupted invariant cannot persist. On success the health overlay
//            clears; after kReinitEscalateAfter consecutive re-init FAILURES the
//            instance escalates to `refuse`.
//   refuse — the instance is PULLED from service: subsequent process() calls
//            fail fast (they never enter plugin code) with a structured error and
//            the instance shows `failed`/`quarantined` in the health contract,
//            until an explicit operator action (re-committing the instance's
//            config — set_instance_def / commit_group) re-enables it.
//
// This is header-only + trivially unit-testable (no Engine dependency), matching
// xi_health.hpp. The declaration home is plugin.json (`"on_fault"`, per-plugin
// default) with an instance.json override, exactly like the other dispatch knobs
// (reentrant / sink from plugin.json, max_concurrency / group from instance.json).
//
#include <string>
#include <string_view>

namespace xi {

// Per-instance post-fault policy. `Reuse` is the wire/default value.
enum class OnFault { Reuse, Reinit, Refuse };

// Consecutive re-init FAILURES (factory or set_def failing on the rebuilt
// instance) after which a `reinit` instance escalates to `refuse` — a small
// constant so a persistently-unrebuildable instance stops thrashing the lifecycle
// every frame and is pulled from service instead. Documented in
// docs/new_gen/04-health-contract.md and the plugin authoring guide.
inline constexpr int kReinitEscalateAfter = 3;

inline const char* on_fault_name(OnFault p) {
    switch (p) {
        case OnFault::Reuse:  return "reuse";
        case OnFault::Reinit: return "reinit";
        case OnFault::Refuse: return "refuse";
    }
    return "reuse";
}

// Parse the wire string; unknown / empty falls back to `dflt` (so a typo does not
// silently pull an instance from service — it keeps the caller's default).
inline OnFault parse_on_fault(std::string_view s, OnFault dflt = OnFault::Reuse) {
    if (s == "reuse")  return OnFault::Reuse;
    if (s == "reinit") return OnFault::Reinit;
    if (s == "refuse") return OnFault::Refuse;
    return dflt;
}

}  // namespace xi
