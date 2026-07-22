// config_swap_probe example — changing a plugin's config WITHOUT stalling or
// tearing the pipeline.
//
// The problem is old and boring and it bites everyone: a plugin holds a heavy
// resource (model weights, a calibration table, an SBM template) and you need
// to swap it while product is moving. Load it on the live pointer and every
// frame in flight stalls behind the load — or worse, one frame reads half the
// old table and half the new one and you get a defect you will never reproduce.
//
// The answer is a DOUBLE SLOT, and config_swap_probe is the reference for it:
//
//   active_   the LIVE slot process() reads. An atomic pointer.
//   staged_   the BACKGROUND slot. prepare() builds the new (expensive)
//             resource here, concurrently with live traffic, touching nothing
//             the running pipeline reads.
//   commit()  one atomic pointer swap: active_ = staged_.
//
// The host drives it in two steps that this example makes visible frame by
// frame:
//
//   prepare_instance {value:99}   -> prepare(): stage in the background. Live
//                                    traffic KEEPS RUNNING on 42. This is the
//                                    part people expect to be a stall, and is
//                                    not.
//   commit_group ["probe"]        -> the host drains dispatch, then calls
//                                    commit() inside a no-process window, then
//                                    resumes. There is no frame in flight
//                                    across the swap, so there is no frame that
//                                    can see half of it.
//
// What this script does per trigger:
//
//   1. DRIVE  — chain the trigger's own pack into the probe's xi.pack@1 door.
//               The probe's process() is a deliberate no-op observation step:
//               it records which config value the LIVE slot held when this
//               frame ran, and returns an empty (but valid) pack. An empty
//               result with no "$fault" IS success for a door with no output.
//   2. OBSERVE — ask the probe's control plane what it thinks its state is:
//               active / staged / staged_value / last_seen / proc.
//   3. VERDICT — the atomicity check is `last_seen == active`. `last_seen` is
//               what the door saw DURING this frame; `active` is what the live
//               slot holds when we read it back a moment later. If a swap could
//               land mid-frame those two would disagree. They never do.
//
// The three states a frame is allowed to observe, and no others:
//
//     (active=42, staged=0)   before prepare
//     (active=42, staged=1)   staged and loaded, but NOT yet live  <-- the point
//     (active=99, staged=0)   committed
//
// The middle one is the whole design. A staged config is fully built and
// costing nothing; frames keep seeing the old one until someone says commit.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

// The probe's status reply is a tiny flat JSON object ({"active":42,
// "staged":false,...}). Read one numeric field out of it, true/false as 1/0.
// Kept dependency-free on purpose — a script may talk to any plugin's exchange
// surface, and that surface is just a string.
static long long status_num(const std::string& s, const char* key, long long dflt) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return dflt;
    p = s.find(':', p + pat.size());
    if (p == std::string::npos) return dflt;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (p >= s.size()) return dflt;
    if (s.compare(p, 4, "true") == 0)  return 1;
    if (s.compare(p, 5, "false") == 0) return 0;
    return std::atoll(s.c_str() + p);
}

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;

    auto tp = t.pack();
    if (!tp) return;
    const long long seq = (long long)tp.get_i64("seq").value_or(-1);

    // ---- 1. DRIVE: the trigger pack straight through the probe's door ------
    // No adapter, no conversion: the sealed pack the camera produced is the
    // same object the plugin's process() receives. The probe ignores its
    // entries (it has no input contract) and answers with an empty pack.
    auto out = xi::use("probe").process(tp);
    const bool door_ok = out.valid() && !out.get_str("$fault").has_value();

    // ---- 2. OBSERVE: the config/prepare/commit control plane ---------------
    const std::string st = xi::use("probe").exchange("{\"command\":\"get_status\"}");
    const long long active = status_num(st, "active", -1);        // the LIVE slot
    const long long staged = status_num(st, "staged", -1);        // is one waiting?
    const long long sval   = status_num(st, "staged_value", -2);  // what it holds
    const long long last   = status_num(st, "last_seen", -1);     // what THIS frame saw
    const long long proc   = status_num(st, "proc", -1);          // door call count

    // ---- 3. VERDICT: atomicity, per frame ----------------------------------
    // last == active is the frame-perfect claim in one comparison. A staged
    // config must also be fully described (staged=1 <=> a real staged_value),
    // never a half-announced one.
    const bool torn      = (last != active);
    const bool staged_ok = (staged == 1) ? (sval >= 0) : (sval == -1);
    const bool pass = seq >= 0 && door_ok && active >= 0 && staged >= 0 &&
                      !torn && staged_ok && proc >= 1;

    // Surface the phase live so the swap is watchable in the UI, not just
    // provable in a test.
    xi::ScriptPackBuilder e;
    e.add_str("$channel", "swap");
    e.add_i64("$seq", (int64_t)xi::run_id());
    e.add_i64("seq", seq);
    e.add_i64("active", active);
    e.add_i64("staged", staged);
    e.add_i64("staged_value", sval);
    e.add_i64("last_seen", last);
    e.add_i64("torn", torn ? 1 : 0);
    xi::use("expose").push(e.seal());

    char msg[192];
    std::snprintf(msg, sizeof msg,
                  "swap seq=%lld door=%d active=%lld staged=%lld sval=%lld "
                  "last=%lld proc=%lld",
                  seq, door_ok ? 1 : 0, active, staged, sval, last, proc);
    if (pass) xi::ok(1, msg);
    else      xi::ng(1, msg);
}
