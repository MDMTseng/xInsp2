// qa_pack_config_swap — the F1 composing example (docs/new_gen/12-pack-parity-
// matrix.md row F1; docs/new_gen/10 gate P2 owed-example clause): the CONFIG-SWAP
// pattern driven PACK-ONLY. A running graph where config_swap_probe processes
// packs through its xi.pack@1 door while the DRIVER swaps its def mid-run via
// the orchestrator path (prepare_instance → commit_group; set_def tier-1 seeds
// the initial value from instances/probe/instance.json).
//
// Per trigger (mock_camera in PACK MODE drives the pipeline) the script:
//
//   1. PACK DRIVE — chains the trigger's own pack into the probe's pack door:
//      xi::use("probe").process(t.pack()). The probe's door is the honest
//      mirror of its no-op observation step: it reads the LIVE config slot into
//      last_seen, bumps its process counter, returns an EMPTY pack (and, per
//      its README, ignores the trigger pack's entries — unknown-entry
//      tolerance). A valid-but-empty result with no "$fault" entry is door
//      success; an INVALID result would mean miss/no-door/crash.
//   2. OBSERVE — xi::use("probe").exchange({command:get_status}) reads the
//      control-plane status: active / staged / staged_value / last_seen / proc.
//   3. VERDICT (run_result plane) — stamps all of it into the ok/ng message.
//      The frame-perfect claim rides on `last == active`: commit_group's
//      drain-barrier means no run is ever mid-flight across the swap, so the
//      value the door observed (last_seen) always equals the live slot this
//      same run reads back — old XOR new config, never torn.
//
// The driver phases the stream (42 staged-none → 42 staged-99 → 99) and checks
// seq/proc contiguity across the swap (no frame lost or duplicated). The data
// currency here is packs and JSON strings only — the legacy currency's type
// name appears NOWHERE in this file, and the driver greps to enforce that.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

// Tiny flat-JSON field reader for the probe's status reply ({"active":42,
// "staged":false,...}): returns the number after `"key":`, with true/false
// read as 1/0. Keeps the script dependency-free on the status surface.
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
    if (!tp) return;                                 // no pack on this tick → NA
    const long long seq = (long long)tp.get_i64("seq").value_or(-1);

    // ---- 1. PACK DRIVE: the trigger pack through the probe's door ----------
    auto out = xi::use("probe").process(tp);
    const bool fault   = out.get_str("$fault").has_value();
    const bool door_ok = out.valid() && !fault;      // empty-but-valid = success

    // ---- 2. OBSERVE: the config/prepare/commit control plane ---------------
    const std::string st =
        xi::use("probe").exchange("{\"command\":\"get_status\"}");
    const long long active = status_num(st, "active", -1);
    const long long staged = status_num(st, "staged", -1);
    const long long sval   = status_num(st, "staged_value", -2);
    const long long last   = status_num(st, "last_seen", -1);
    const long long proc   = status_num(st, "proc", -1);

    // ---- 3. VERDICT on the run_result plane ---------------------------------
    // last == active is the per-run frame-perfect check (see header comment);
    // sval must mirror the staged flag (staging visible, then consumed).
    const bool pass = seq >= 0 && door_ok && active >= 0 && staged >= 0 &&
                      last == active && proc >= 1 &&
                      (staged == 1 ? sval >= 0 : sval == -1);
    char msg[192];
    std::snprintf(msg, sizeof msg,
                  "cswap seq=%lld door=%d active=%lld staged=%lld sval=%lld "
                  "last=%lld proc=%lld",
                  seq, door_ok ? 1 : 0, active, staged, sval, last, proc);
    if (pass) xi::ok(1, msg);
    else      xi::ng(1, msg);
}
