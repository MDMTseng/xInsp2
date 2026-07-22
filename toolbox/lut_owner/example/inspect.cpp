// lut_owner example — a calibration table that never rides a pack.
//
// The problem: your inspection needs a big lookup structure — a calibration
// curve, a colour LUT, an index, model weights. It is expensive to build and
// cheap to query, and you need it on every frame. Serialising it into a pack
// per frame is absurd; passing a raw pointer across the plugin ABI is worse.
//
// The answer is the RESOURCE-HANDLE convention (docs/new_gen/14 appendix), and
// `lut_owner` is its executable reference. It is a LIB plugin — no data plane,
// nothing routes to it — that owns the objects and publishes three
// capabilities: demo.lut.build / .query / .dump. What crosses the ABI is never
// the table, only a HANDLE ENTRY:
//
//     { "type": "demo.lut", "id": <slot>, "gen": <generation>, "$v": 1 }
//
// This script holds one of those between ticks. Read it as three lessons:
//
//   BUILD ONCE, QUERY MANY. The table is built on the first tick only. Every
//   later tick sends the same handle back. The owner's lifetime `builds`
//   counter stays at 1 for the whole run — that is the number the driver
//   checks, and it is the entire economic argument for the pattern.
//
//   A HANDLE IS A LEASE, NOT A POINTER. Slots are recycled under pressure and
//   every recycle bumps a monotonic generation, so an old handle can never
//   silently alias a new object: it resolves to a clean sealed $fault
//   "stale_handle". Losing your lease is NORMAL and you must code for it —
//   here, by rebuilding on the spot. (Send `lut` the exchange command
//   {"command":"recycle_all"} while this runs and watch that happen.)
//
//   THE PROVIDER IS OPTIONAL. If no demo.lut instance is loaded, the consumer
//   says so (have_cap=0) and the run reports "not applicable" instead of
//   failing. A capability you cannot run without is not a capability, it is a
//   dependency.
//
// Note that the script talks to `grader` over the ordinary PACK plane. Scripts
// do not call capabilities; plugins do. `grader` (plugins/lut_client) is the
// consumer — about forty lines, worth reading, it is the whole API.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_mp.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

// The "calibration table": measured code -> grade. Nine entries here because
// the point is the plumbing; imagine 100k.
constexpr int kEntries = 9;

xi::mp::Writer table_keys() {
    xi::mp::Writer w; w.array(kEntries);
    for (int i = 1; i <= kEntries; ++i) w.int_(i * 100);
    return w;
}
xi::mp::Writer table_values() {
    xi::mp::Writer w; w.array(kEntries);
    for (int i = 1; i <= kEntries; ++i) w.int_(i * 10);
    return w;
}
xi::mp::Writer one_key(int64_t k) {
    xi::mp::Writer w; w.array(1); w.int_(k);
    return w;
}

// First element of the provider's mp values array, if it is an int.
std::optional<int64_t> first_value(std::optional<std::span<const uint8_t>> mp) {
    if (!mp) return std::nullopt;
    xi::mp::Reader r(mp->data(), mp->size());
    xi::mp::Element e;
    if (r.next(e) != xi::mp::Status::Ok || e.kind != xi::mp::Kind::Array || e.len < 1)
        return std::nullopt;
    xi::mp::Element v;
    if (r.next(v) != xi::mp::Status::Ok || v.kind != xi::mp::Kind::Int)
        return std::nullopt;
    return v.i;
}

// The handle we are leasing, carried across ticks. Empty = we need to build.
std::vector<uint8_t> g_handle;

}  // namespace

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (t.is_active()) return;              // driven by the synthetic timer tick

    static long long seq = 0;
    const long long s = seq++;

    // This tick's "measurement" and the grade the table owes us for it.
    const int64_t code     = 100 * (1 + s % kEntries);
    const int64_t expected = code / 10;

    // One trip to the consumer door. `with_build` attaches the build leg; we
    // only pay for it when we have no valid lease.
    auto ask = [&](bool with_build) {
        xi::ScriptPackBuilder b;
        if (with_build) {
            b.add_mp("bkeys", table_keys());
            b.add_mp("bvals", table_values());
        } else {
            b.add_mp("handle", g_handle.data(), g_handle.size());
        }
        b.add_mp("qkeys", one_key(code));
        auto req = b.seal();
        return xi::use("grader").process(req);
    };

    auto r = ask(g_handle.empty());

    // -- the provider is optional -------------------------------------------
    if (r.get_i64("have_cap").value_or(0) == 0) {
        xi::result(0, "no demo.lut provider loaded — ungraded, not a defect");
        return;
    }

    // -- the lease expired: rebuild and carry on ----------------------------
    int stale = 0;
    if (r.get_str("q_fault").value_or("") == "stale_handle") {
        stale = 1;
        g_handle.clear();
        r = ask(true);
    }

    // Keep whatever handle came back (a build leg mints a fresh one).
    if (auto h = r.get_mp("handle"); h && !h->empty())
        g_handle.assign(h->begin(), h->end());

    const auto grade   = first_value(r.get_mp("values"));
    const long long bc = r.get_i64("q_builds")
                             .value_or(r.get_i64("builds").value_or(-1));
    const std::string qfault(r.get_str("q_fault").value_or(""));

    char msg[192];
    std::snprintf(msg, sizeof msg,
                  "seq=%lld code=%lld grade=%lld builds=%lld stale=%d",
                  s, (long long)code, (long long)grade.value_or(-1), bc, stale);

    if (!qfault.empty())            xi::ng(1, ("lut fault: " + qfault).c_str());
    else if (!grade)                xi::ng(1, "no grade came back for this code");
    else if (*grade != expected)    xi::ng(1, msg);
    else                            xi::ok(1, msg);
}
