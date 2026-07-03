//
// service_cmd_observability.cpp — observability command handlers, split from
// service_main.cpp (behavior-preserving; see service_internal.hpp).
//
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <yyjson.h>
#include <xi/xi_metrics.hpp>
#include <xi/xi_graph_capture.hpp>

#include "service_internal.hpp"

// Read a file with a hard size cap (review 09 finding 4). Peeks the on-disk size
// first so an oversized file is refused BEFORE the allocation — the whole point
// is to never slurp an unbounded/pathological file into a std::string (which,
// past the dispatch shell, would be a bad_alloc → backend death). On over-cap:
// truncated=true, content left empty (a partial body is invalid JSON, so callers
// report the truncation rather than embed it).
bool read_file_capped(const std::filesystem::path& p, size_t cap,
                      std::string& content, bool& truncated) {
    truncated = false;
    content.clear();
    std::error_code ec;
    auto sz = std::filesystem::file_size(p, ec);
    if (ec) return false;   // missing / not a regular file
    if (sz > cap) { truncated = true; return true; }
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    content.resize((size_t)sz);
    if (sz) f.read(content.data(), (std::streamsize)sz);
    content.resize((size_t)f.gcount());
    return true;
}

// ---- observability ---------------------------------------------------------
void cmd_crash_reports_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // List crash JSON reports left by previous fatal crashes.
        // Returns the file contents inline (each is small, KB-sized).
        namespace fs = std::filesystem;
        auto dir = fs::temp_directory_path() / "xinsp2" / "crashdumps";
        std::string out = "{\"reports\":[";
        bool first = true;
        std::error_code ec;
        if (fs::exists(dir, ec)) {
            std::vector<fs::directory_entry> entries;
            for (auto& e : fs::directory_iterator(dir, ec)) {
                if (e.path().extension() == ".json") entries.push_back(e);
            }
            // Sort newest-first by mtime
            std::sort(entries.begin(), entries.end(),
                [](auto& a, auto& b) {
                    std::error_code ec2;
                    return fs::last_write_time(a.path(), ec2) > fs::last_write_time(b.path(), ec2);
                });
            for (auto& e : entries) {
                std::string body; bool truncated = false;
                if (!read_file_capped(e.path(), kMaxInlineFileBytes, body, truncated)) continue;
                if (truncated) {
                    // An over-cap crash file: surface its existence + the truncation
                    // instead of embedding a partial (invalid-JSON) body.
                    if (!first) out += ",";
                    first = false;
                    out += "{\"file\":";
                    xp::json_escape_into(out, e.path().filename().string());
                    out += ",\"truncated\":true}";
                    continue;
                }
                while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
                if (body.empty() || body[0] != '{') continue;
                if (!first) out += ",";
                first = false;
                out += "{\"file\":";
                xp::json_escape_into(out, e.path().filename().string());
                out += ",\"report\":";
                out += body;
                out += "}";
            }
        }
        out += "]}";
        send_rsp_ok(srv, id, out);
}

void cmd_clear_crash_reports_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        namespace fs = std::filesystem;
        auto dir = fs::temp_directory_path() / "xinsp2" / "crashdumps";
        int n = 0;
        std::error_code ec;
        if (fs::exists(dir, ec)) {
            for (auto& e : fs::directory_iterator(dir, ec)) {
                fs::remove(e.path(), ec);
                ++n;
            }
        }
        send_rsp_ok(srv, id, "{\"removed\":" + std::to_string(n) + "}");
}

void cmd_set_watchdog_ms_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // P2.4. Set the wall-clock budget (ms) for a single inspect()
        // call. 0 disables. Tripping the watchdog does not auto-reset —
        // the next inspect re-arms with the new budget.
        auto ms_opt = xp::get_number_field(parsed->args_json, "ms");
        int ms = ms_opt ? (int)*ms_opt : 0;
        if (ms < 0) ms = 0;
        if (ms > 600000) ms = 600000;     // 10-minute hard cap
        g_eng.watchdog_ms = ms;
        std::string out = "{\"ms\":" + std::to_string(ms);
        out += ",\"trips\":" + std::to_string(g_eng.watchdog_trips.load()) + "}";
        send_rsp_ok(srv, id, out);
}

/* [cmd_watchdog_status_ RETIRED at THE CUT (v12) — app-team confirmed, doc 11.
 * set_watchdog_ms still returns the same {ms,trips} snapshot on set.] */

void cmd_graph_capture_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Toggle pipeline-graph dataflow capture (stage 2). Default off → no
        // hot-path cost. Enabling clears any prior recording.
        bool enable = parsed->args_json.find("\"enable\":true")  != std::string::npos ||
                      parsed->args_json.find("\"enable\": true") != std::string::npos;
        xi::GraphCapture::instance().set(enable);
        send_rsp_ok(srv, id, std::string("{\"capturing\":") + (enable ? "true" : "false") + "}");
}

void cmd_graph_snapshot_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Reconstruct dataflow EDGES (xi::GraphCapture owns the algorithm) and
        // format the result to wire JSON. Returns the instances that actually
        // ran + the edges.
        auto snap = xi::GraphCapture::instance().snapshot();
        std::string out = "{\"capturing\":";
        out += snap.capturing ? "true" : "false";
        out += ",\"ran\":[";
        for (size_t i = 0; i < snap.ran.size(); ++i) {
            if (i) out += ",";
            out += xp::json_escape(snap.ran[i]);     // json_escape() already wraps in quotes
        }
        out += "],\"edges\":[";
        bool first = true;
        for (auto& e : snap.edges) {
            if (!first) out += ","; first = false;
            out += "{\"from\":" + xp::json_escape(e.from) +
                   ",\"to\":"   + xp::json_escape(e.to) + ",\"keys\":[";
            bool k1 = true;
            for (auto& k : e.keys) { if (!k1) out += ","; k1 = false; out += xp::json_escape(k); }
            out += "]}";
        }
        out += "]}";
        send_rsp_ok(srv, id, out);
}

void cmd_get_state_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Orchestrator read (task #67): the host-tracked instance state machine
        // (created / active / faulted) + last error. Coarse by design — fine
        // staging/ready sub-state is plugin-side (exchange get_status). An
        // instance that exists but has had no host-visible transition yet reads
        // "created". args: { "name": "cam0" } → data: { state, last_error }.
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        InstState st; std::string err; long long crashes = 0;
        bool known = g_eng.plugin_mgr.get_instance_state(*iname, st, err, &crashes);
        if (!known) {
            // No tracked transition — report "created" if the instance actually
            // exists, else a clean not-found.
            if (xi::InstanceRegistry::instance().find(*iname)) { st = InstState::Created; }
            else { send_rsp_err(srv, id, "instance not found: " + *iname); return; }
        }
        std::string data = std::string("{\"state\":\"") + inst_state_str(st) + "\",\"last_error\":";
        xp::json_escape_into(data, err);
        // crash_count: a process() crash leaves the instance Active + returns NA, so
        // this is how a host detects a per-instance crash loop and alerts.
        data += ",\"crash_count\":" + std::to_string(crashes);
        data += "}";
        send_rsp_ok(srv, id, data);
}

void cmd_recent_errors_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Return error events captured by the cross-channel ring
        // (rsp.error / log level=error / async event etc).
        // Optional `since_ms` arg filters out older entries — useful
        // for "any errors since I sent my last cmd?" polling.
        auto since_opt = xp::get_number_field(parsed->args_json, "since_ms");
        int64_t since = since_opt ? (int64_t)*since_opt : 0;
        std::string out = "[";
        {
            std::lock_guard<std::mutex> lk(g_eng.recent_errors_mu);
            int n = 0;
            for (auto& e : g_eng.recent_errors) {
                if (e.ts_ms < since) continue;
                if (n++) out += ",";
                out += "{\"ts_ms\":" + std::to_string(e.ts_ms);
                out += ",\"source\":"; xp::json_escape_into(out, e.source);
                out += ",\"message\":"; xp::json_escape_into(out, e.message);
                if (e.cmd_id) out += ",\"cmd_id\":" + std::to_string(e.cmd_id);
                if (e.run_id) out += ",\"run_id\":" + std::to_string(e.run_id);
                out += "}";
            }
        }
        out += "]";
        send_rsp_ok(srv, id, out);
}

void cmd_status_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Snapshot of every component's latest sticky status. Clients call this
        // on EVERY (re)connect — that re-pull over the retained map is what
        // guarantees the latest status always arrives, even across reconnects
        // and backend respawns; the `status` push event is just a low-latency
        // accelerator between snapshots.
        std::string out = "{";
        {
            std::lock_guard<std::mutex> lk(g_eng.status_mu);
            int n = 0;
            for (auto& [who, e] : g_eng.status) {
                if (n++) out += ",";
                xp::json_escape_into(out, who);
                out += ":{\"text\":"; xp::json_escape_into(out, e.text);
                out += ",\"ts_ms\":" + std::to_string(e.ts_ms);
                out += ",\"seq\":" + std::to_string(e.seq) + "}";
            }
        }
        out += "}";
        send_rsp_ok(srv, id, out);
}

void cmd_image_pool_stats_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Per-owner ImagePool footprint. Owner IDs alone are
        // meaningless to humans — we look them up against the
        // running project's instances + script so the reply names
        // who is holding the memory. Anonymous (owner == 0) is
        // collapsed under "label":"<host>".
        auto totals   = xi::ImagePool::instance().stats();
        auto by_owner = xi::ImagePool::instance().stats_by_owner();

        // Build owner_id → label map.
        std::unordered_map<xi::ImagePoolOwnerId, std::string> labels;
        labels[0] = "<host>";
        {
            std::lock_guard<std::mutex> lk(g_eng.script_mu);
            if (g_eng.script.owner_id != 0) {
                labels[g_eng.script.owner_id] =
                    "script:" + std::filesystem::path(g_eng.script.path).filename().string();
            }
        }
        for (auto& [iname, ii] : g_eng.plugin_mgr.project().instances) {
            if (auto* a = dynamic_cast<xi::CAbiInstanceAdapter*>(ii.instance.get())) {
                labels[a->owner_id()] = "instance:" + ii.name + " (" + ii.plugin_name + ")";
            }
            // All instances are in-process CAbiInstanceAdapters now;
            // process-isolation + SHM were removed 2026-05.
        }

        auto label_for = [&](xi::ImagePoolOwnerId o) -> std::string {
            auto it = labels.find(o);
            if (it != labels.end()) return it->second;
            return "owner:" + std::to_string(o) + " (orphan)";
        };

        // Cumulative diagnostics: total_created and high_water never
        // decrement, so they expose activity even when live counts are
        // zero between runs (the agent feedback loop hit this — live
        // snapshots looked empty mid-test, hiding real allocation).
        auto cum = xi::ImagePool::instance().cumulative();
        std::string out = "{\"total\":{\"handles\":"
                        + std::to_string(totals.handle_count)
                        + ",\"bytes\":"
                        + std::to_string(totals.total_bytes)
                        + "},\"cumulative\":{\"total_created\":"
                        + std::to_string(cum.total_created)
                        + ",\"high_water\":"
                        + std::to_string(cum.high_water)
                        + ",\"live_now\":"
                        + std::to_string(cum.live_now)
                        + "},\"by_owner\":[";
        for (size_t i = 0; i < by_owner.size(); ++i) {
            if (i) out += ",";
            out += "{\"owner\":"   + std::to_string(by_owner[i].owner)
                +  ",\"label\":";
            xp::json_escape_into(out, label_for(by_owner[i].owner));
            out +=  ",\"handles\":" + std::to_string(by_owner[i].handle_count)
                +  ",\"bytes\":"    + std::to_string(by_owner[i].total_bytes)
                +  "}";
        }
        out += "]}";
        send_rsp_ok(srv, id, out);
}

void cmd_dispatch_stats_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Snapshot of queue health. Useful for drivers / agents that
        // want to know if their source is overproducing.
        //
        // Field semantics:
        //   queue_depth_now             — current queue size
        //   queue_depth_cap             — configured project.json cap
        //   queue_depth_high_watermark  — peak depth observed since
        //                                 last cmd:start (real obs.)
        //   dropped_oldest / dropped_newest — overflow counters since
        //                                 last cmd:start
        //
        // ALL THREE COUNTERS (high_watermark, dropped_oldest,
        // dropped_newest) are reset by cmd:start. Drivers that snapshot
        // before AND after a start will see the AFTER values come back
        // smaller than BEFORE — don't subtract. See docs/reference/ws-protocol.md
        // `dispatch_stats` for the public contract.
        //
        // P1-8: `dropped_lifetime` and `queue_depth_high_watermark_lifetime` are the
        // process-uptime cumulatives — they do NOT reset at cmd:start, so a monitor
        // can answer "how much have we dropped total" / "peak depth ever" across run
        // boundaries (the per-run counters above answer "...since this start").
        std::string data;
        // Snapshot the lanes under g_eng.lanes_mu so a concurrent stop can't free
        // them mid-read. A no-groups project has one synthesized default lane, so
        // the top-level totals are aggregates across all lanes (unified dispatch).
        std::vector<std::shared_ptr<GroupLane>> lanes;
        { std::lock_guard<std::mutex> lk(g_eng.lanes_mu); lanes = g_eng.lanes; }
        size_t qsz = 0; uint64_t hw = 0, dropped = 0;
        for (auto& lp : lanes) {
            { std::lock_guard<std::mutex> lk(lp->mu); qsz += lp->q.size(); }
            hw += lp->high_watermark.load();
            dropped += lp->dropped.load();
        }
        data  = "{\"queue_depth_now\":" + std::to_string(qsz);
        data += ",\"queue_depth_cap\":" + std::to_string(g_eng.plugin_mgr.project().queue_depth);
        data += ",\"queue_depth_high_watermark\":" + std::to_string(hw);
        data += ",\"overflow\":\"" + g_eng.plugin_mgr.project().overflow + "\"";
        data += ",\"dispatch_threads\":" + std::to_string(g_eng.plugin_mgr.project().dispatch_threads);
        data += ",\"dropped\":" + std::to_string(dropped);
        // P1-8: process-uptime cumulatives (NOT reset by cmd:start).
        data += ",\"dropped_lifetime\":" + std::to_string(g_eng.dropped_lifetime.load());
        data += ",\"queue_depth_high_watermark_lifetime\":" + std::to_string(g_eng.high_watermark_lifetime.load());
        // Review 09 finding 2: malformed/unparseable command envelopes rejected by
        // the dispatch shell (process-uptime cumulative, like the *_lifetime fields).
        data += ",\"malformed_cmd_rejected_lifetime\":" + std::to_string(g_eng.malformed_cmd_rejected.load());
        // Q0f: host-owned metadata docs deliberately leaked at a caught plugin crash
        // (rc -2, leak-over-UAF). Process-uptime cumulative (NOT reset by cmd:start,
        // like the other *_lifetime fields). A steadily-climbing value means a plugin
        // is crashing repeatedly on doc-carrying calls; a constant non-zero value is a
        // one-off. Bounded by the on_fault quarantine; dissolved by the v3 pack plane.
        data += ",\"crash_leaked_docs_lifetime\":" + std::to_string(xi::DocRegistry::instance().crash_leaked_lifetime());
        // Source liveness: ms since ANY source last emitted, + per-source ages. The
        // signal for "a camera stalled" — a stalled source otherwise stops the line
        // with zero indication. -1 = nothing has emitted yet. A monitor/FE applies a
        // source-rate-appropriate staleness threshold (auto-alerting on a fixed
        // threshold is left to the consumer since the expected rate is source-specific).
        {
            int64_t age_us = xi::TriggerBus::instance().last_emit_age_us();
            data += ",\"last_emit_age_ms\":" + (age_us < 0 ? std::string("-1")
                                              : std::to_string(age_us / 1000));
            auto ages = xi::TriggerBus::instance().source_emit_ages_us();
            data += ",\"sources\":[";
            for (size_t i = 0; i < ages.size(); ++i) {
                if (i) data += ",";
                data += "{\"source\":"; xp::json_escape_into(data, ages[i].first);
                data += ",\"last_emit_age_ms\":" + std::to_string(ages[i].second / 1000) + "}";
            }
            data += "]";
        }
        // Per-group lanes (always ≥1: the default lane when no groups declared).
        if (!lanes.empty()) {
            data += ",\"groups\":[";
            for (size_t i = 0; i < lanes.size(); ++i) {
                auto& l = *lanes[i];
                size_t lq; { std::lock_guard<std::mutex> lk(l.mu); lq = l.q.size(); }
                if (i) data += ",";
                data += "{\"name\":"; xp::json_escape_into(data, l.cfg.name);
                data += ",\"max_parallel\":" + std::to_string(l.cfg.max_parallel);
                data += ",\"thread_priority\":\"" + l.cfg.thread_priority + "\"";
                data += ",\"min_interval_ms\":" + std::to_string(l.cfg.min_interval_ms);
                data += ",\"queue_now\":" + std::to_string(lq);
                data += ",\"running\":" + std::to_string(l.running.load());
                data += ",\"high_watermark\":" + std::to_string(l.high_watermark.load());
                data += ",\"dropped\":" + std::to_string(l.dropped.load());
                data += ",\"cpu_affinity\":[";   // [] = unbound; else the per-worker mask sets
                for (size_t si = 0; si < l.cfg.cpu_affinity.size(); ++si) {
                    if (si) data += ",";
                    data += "[";
                    for (size_t ci = 0; ci < l.cfg.cpu_affinity[si].size(); ++ci) {
                        if (ci) data += ",";
                        data += std::to_string(l.cfg.cpu_affinity[si][ci]);
                    }
                    data += "]";
                }
                data += "]}";
            }
            data += "]";
        }
        data += "}";
        send_rsp_ok(srv, id, data);
}

void cmd_metrics_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // OQ-7a observability export (core_fix_plan §21 / §27.5 "don't gold-plate").
        // The minimal, honest metrics surface: monotonic per-frame counters
        // (total/ok/error) + a fixed-bucket per-frame latency histogram, recorded
        // in run_one_inspection. Point-query snapshot over THIS existing WS channel
        // (same shape/role as dispatch_stats / image_pool_stats) — no separate
        // telemetry server. Counters are process-uptime cumulative (NOT reset by
        // cmd:start), so a monitor snapshots before/after and derives its own rates
        // (do not subtract across a restart — same caveat as dispatch_stats).
        std::string data;
        xi::MetricsRegistry::instance().snapshot_json(data);
        send_rsp_ok(srv, id, data);
}

void cmd_open_project_warnings_(xi::ws::Server& srv, int64_t id, const xp::ParsedCmd* parsed) {
        // Returns the per-instance warnings collected during the most
        // recent open_project. open_project itself succeeds even when
        // individual instances fail (skip-bad-instance), so this is
        // how a UI / agent surfaces those problems instead of having
        // to scrape backend stderr.
        auto warnings = g_eng.plugin_mgr.open_warnings();
        std::string data = "{\"warnings\":[";
        bool first = true;
        for (auto& w : warnings) {
            if (!first) data += ",";
            first = false;
            data += "{\"instance\":";
            xp::json_escape_into(data, w.instance);
            data += ",\"plugin\":";
            xp::json_escape_into(data, w.plugin);
            data += ",\"reason\":";
            xp::json_escape_into(data, w.reason);
            data += "}";
        }
        data += "]}";
        send_rsp_ok(srv, id, data);
}

