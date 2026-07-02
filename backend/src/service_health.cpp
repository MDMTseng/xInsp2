//
// service_health.cpp — the core-owned health/state contract's service surface
// (adoption-map item 10 / triage Bucket E). Design: docs/new_gen/04-health-contract.md.
//
// The state machine + the stored health (top-level state, script health, the
// per-instance runtime-fault overlay) live in the header-only xi::HealthRegistry
// (xi_health.hpp), so they are unit-testable without the Engine. THIS TU owns the
// two things that need g_eng:
//
//   * install_health_notifier_() — routes HealthRegistry's health_changed events
//     to the WS clients (called once at boot, like status_sink / binary_sink).
//   * cmd_get_health_() — assembles the get_health reply: the registry's stored
//     state/script/overlay, MERGED with the components whose truth lives in their
//     existing owners (instances → PluginManager InstState; groups → the dispatch
//     lanes; sources → TriggerBus emit ages). Nothing here is a second copy of
//     that state — it is read at query time so it cannot drift.
//
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "service_internal.hpp"
#include <xi/xi_health.hpp>

// FE mirror: the top-level health, written to a tiny file the FE supervisor polls
// (it can't read get_health — that would consume the single WS client slot the
// HMI/extension needs; see docs/internals/fe-be.md). Set once at boot from
// install_health_notifier_(); empty = no mirror (a bare/manually-run BE).
static std::string g_health_mirror_path;

// Write the mirror atomically (tmp + rename) so a mid-write poll never reads a
// torn file. Lifecycle-rate (only on health_changed), so the I/O is cheap and off
// the per-frame path. Best-effort — a failed mirror must never affect the BE.
static void write_health_mirror_() {
    if (g_health_mirror_path.empty()) return;
    std::string js  = xi::health().mirror_json();
    std::string tmp = g_health_mirror_path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << js;
        if (!f) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, g_health_mirror_path, ec);  // replaces on Win/POSIX
    if (ec) {   // cross-volume or locked: fall back to a direct (non-atomic) write
        std::ofstream f(g_health_mirror_path, std::ios::binary | std::ios::trunc);
        if (f) f << js;
        std::filesystem::remove(tmp, ec);
    }
}

// Route HealthRegistry state changes to WS clients AND mirror the top-level state
// to the FE's status file. The event JSON is already built by the registry; we
// just push it (best-effort, like the `status` push) and re-write the mirror.
// Installed once from main(). `health_file` empty disables the mirror.
void install_health_notifier_(const std::string& health_file) {
    g_health_mirror_path = health_file;
    xi::health().set_notifier([](const std::string& event_json) {
        if (auto* s = g_eng.srv_for_bp.load(std::memory_order_acquire))
            s->send_text(event_json);
        write_health_mirror_();
    });
    // Publish the initial state (boot) immediately so the FE has a value to read
    // before the first transition, mirroring the FE's own initial status write.
    write_health_mirror_();
}

void cmd_get_health_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // The one canonical health read (subsumes get_state / status / the
        // dispatch_stats liveness fields). Point-query snapshot, same role as
        // dispatch_stats / image_pool_stats / metrics.
        auto& H = xi::health();

        xi::SysState st; int64_t since = 0;
        H.state_snapshot(st, since);

        std::string out = "{\"schema\":";
        xi::health_json_str(out, xi::kHealthSchema);
        out += ",\"state\":";
        xi::health_json_str(out, xi::sys_state_name(st));
        out += ",\"since_ms\":" + std::to_string(since);
        out += ",\"boot_id\":";    xi::health_json_str(out, g_eng.boot_id);
        out += ",\"station_id\":"; xi::health_json_str(out, g_eng.station_id);
        out += ",\"components\":[";
        bool first = true;
        auto sep = [&]() { if (!first) out += ","; first = false; };

        // --- script: the registry owns this one -------------------------------
        {
            std::string sname, sreason; xi::CompHealth sh; int64_t ssince = 0;
            if (H.script_snapshot(sname, sh, sreason, ssince)) {
                sep();
                xi::HealthRegistry::append_component(out, xi::kKindScript, sname, sh,
                                                     sreason, ssince);
            }
        }

        // --- instances: base = PluginManager InstState, OVERLAID with the
        //     registry's runtime-fault set. The PluginManager owns the base state
        //     (and migrates it atomically on rename/remove), so we read it here
        //     rather than keeping a parallel copy that could drift.
        for (auto& [iname, ii] : g_eng.plugin_mgr.project().instances) {
            InstState base = InstState::Created;
            std::string err; long long crashes = 0;
            if (!g_eng.plugin_mgr.get_instance_state(iname, base, err, &crashes)) {
                // No tracked transition yet → Created (born state). Still report it.
                base = InstState::Created;
            }
            xi::CompHealth h = xi::CompHealth::Ok;
            std::string reason; int64_t comp_since = 0;
            std::string ovr_reason; int64_t ovr_since = 0;
            if (base == InstState::Faulted) {
                h = xi::CompHealth::Failed;
                reason = xi::kReasonPrepareFailed;
            } else if (H.instance_degraded(iname, ovr_reason, ovr_since)) {
                // A caught runtime crash (InstState stays Active) — the quarantine
                // seed. Overlay wins over the ok base.
                h = xi::CompHealth::Degraded;
                reason = ovr_reason;
                comp_since = ovr_since;
            }
            std::string extra = ",\"crash_count\":" + std::to_string((long long)crashes);
            sep();
            xi::HealthRegistry::append_component(out, xi::kKindInstance, iname, h,
                                                 reason, comp_since, extra);
        }

        // --- dispatch groups: a lane exists ⇒ ok. Derived from g_eng.lanes; carry
        //     the same queue/running/dropped a dispatch_stats reader would want.
        {
            std::vector<std::shared_ptr<GroupLane>> lanes;
            { std::lock_guard<std::mutex> lk(g_eng.lanes_mu); lanes = g_eng.lanes; }
            for (auto& lp : lanes) {
                size_t qn; { std::lock_guard<std::mutex> lk(lp->mu); qn = lp->q.size(); }
                std::string extra = ",\"queue_now\":" + std::to_string(qn)
                                  + ",\"running\":" + std::to_string(lp->running.load())
                                  + ",\"dropped\":" + std::to_string(lp->dropped.load());
                sep();
                xi::HealthRegistry::append_component(out, xi::kKindGroup, lp->cfg.name,
                                                     xi::CompHealth::Ok, "", 0, extra);
            }
        }

        // --- sources: liveness = the EXISTING TriggerBus emit-age tracking (no
        //     second staleness tracker). Always ok — core does not invent a
        //     staleness threshold (rate is source-specific; alerting is the
        //     consumer's call, same stance as dispatch_stats).
        {
            auto ages = xi::TriggerBus::instance().source_emit_ages_us();
            for (auto& [sname, age_us] : ages) {
                std::string extra = ",\"last_emit_age_ms\":" + std::to_string(age_us / 1000);
                sep();
                xi::HealthRegistry::append_component(out, xi::kKindSource, sname,
                                                     xi::CompHealth::Ok, "", 0, extra);
            }
        }

        out += "]}";
        send_rsp_ok(srv, id, out);
}
