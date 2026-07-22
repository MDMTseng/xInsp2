// qa_resource_handle — the resource-handle convention LIVE (doc 14 appendix):
// heavy objects never ride packs; the demo.lut TYPE OWNER (lut_owner lib
// instance, ring_slots=2) constructs them, and packs carry only the handle
// entry {type:"demo.lut", id, gen, $v} as a nested canonical-mp map.
//
// Per tick (synthetic timer drives — no camera needed):
//   1. u1 (consumer 1): build the FIXED LUT A (content-identical every tick),
//      query it, dump it. Assert built=1 exactly once (tick 0) and 0 forever
//      after — the owner's content dedup — and remember the returned handle
//      entry H (raw mp bytes).
//   2. THE DOOR HOP: the script places H VERBATIM into a new pack and hands it
//      to u2 (consumer 2), which queries + dumps THROUGH IT. Assert: correct
//      values, the owner's build counter UNCHANGED between the two consumers
//      (zero rebuild — the headline), and u1's dump bytes == u2's dump bytes
//      (the materializer is byte-deterministic).
//   3. STALE LEASE: u1 builds a unique throwaway LUT T_seq; with ring_slots=2
//      the ring is now {A(hot), T_seq}, so building T_seq evicted T_{seq-1}
//      (LRU recycle = the forced-recycle path). Querying LAST tick's throwaway
//      handle must answer a clean sealed $fault "stale_handle" (funnel rc 0).
//   4. WRONG TYPE: a synthesized handle {type:"demo.image", ...} must answer
//      $fault "wrong_type".
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_mp.hpp>
#include <xi/xi_result.hpp>

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

xi::mp::Writer i64_array(std::initializer_list<int64_t> xs) {
    xi::mp::Writer w;
    w.array((uint32_t)xs.size());
    for (int64_t x : xs) w.int_(x);
    return w;
}

// Decode an i64-or-nil values array; true iff it equals `want` with no nils.
bool values_equal(std::optional<std::span<const uint8_t>> mp,
                  std::initializer_list<int64_t> want) {
    if (!mp) return false;
    xi::mp::Reader r(mp->data(), mp->size());
    xi::mp::Element e;
    if (r.next(e) != xi::mp::Status::Ok || e.kind != xi::mp::Kind::Array ||
        e.len != want.size())
        return false;
    for (int64_t w : want) {
        xi::mp::Element el;
        if (r.next(el) != xi::mp::Status::Ok ||
            el.kind != xi::mp::Kind::Int || el.i != w)
            return false;
    }
    return true;
}

} // namespace

XI_INSPECT_ENTRY(t, frame) {
    (void)frame; (void)t;
    static long long run = 0;
    static std::vector<uint8_t> t_prev;   // last tick's throwaway handle bytes
    const long long seq = run++;

    bool flt = false;   // any unexpected $fault / rc on the happy legs

    // ---- 1. consumer 1: build the FIXED LUT A + query + dump ----------------
    xi::ScriptPackBuilder b1;
    bool built_ok = b1.valid();
    built_ok = b1.add_mp("bkeys", i64_array({10, 20, 30, 40})) && built_ok;
    built_ok = b1.add_mp("bvals", i64_array({11, 22, 33, 44})) && built_ok;
    built_ok = b1.add_mp("qkeys", i64_array({20, 40})) && built_ok;
    built_ok = b1.add_i64("dump", 1) && built_ok;
    auto p1 = b1.seal();
    built_ok = built_ok && p1.valid();

    auto r1 = xi::use("u1").process(p1);
    flt = flt || r1.is_fault();
    const long long built  = r1.get_i64("built").value_or(-1);
    const long long b_cnt  = r1.get_i64("builds").value_or(-1);
    const bool rc1_ok = r1.get_i64("build_rc").value_or(-99) == 0 &&
                        r1.get_i64("query_rc").value_or(-99) == 0 &&
                        r1.get_i64("dump_rc").value_or(-99) == 0;
    const bool clean1 = r1.get_str("b_fault").value_or("x").empty() &&
                        r1.get_str("q_fault").value_or("x").empty() &&
                        r1.get_str("d_fault").value_or("x").empty();
    const bool q1 = values_equal(r1.get_mp("values"), {22, 44}) &&
                    r1.get_i64("found").value_or(-1) == 2 &&
                    r1.get_i64("q_builds").value_or(-1) == b_cnt;
    auto handle = r1.get_mp("handle");
    auto dump1  = r1.get_bin("lut");
    const bool hop = handle.has_value() && !handle->empty();
    // built is 1 exactly on the very first tick, 0 (dedup) forever after.
    const bool zr1 = (seq == 0) ? (built == 1) : (built == 0);

    // ---- 2. THE DOOR HOP: the handle entry rides a fresh pack to consumer 2 --
    bool q2 = false, zr2 = false, dq = false;
    if (hop) {
        xi::ScriptPackBuilder b2;
        bool ok2 = b2.valid();
        ok2 = b2.add_mp("handle", *handle) && ok2;
        ok2 = b2.add_mp("qkeys", i64_array({10, 30})) && ok2;
        ok2 = b2.add_i64("dump", 1) && ok2;
        auto p2 = b2.seal();
        auto r2 = xi::use("u2").process(p2);
        flt = flt || !ok2 || r2.is_fault();
        q2 = r2.get_i64("query_rc").value_or(-99) == 0 &&
             r2.get_str("q_fault").value_or("x").empty() &&
             values_equal(r2.get_mp("values"), {11, 33}) &&
             r2.get_i64("found").value_or(-1) == 2;
        // Zero rebuild across consumers: the owner's build counter at u2's
        // query time equals what u1 saw.
        zr2 = r2.get_i64("q_builds").value_or(-1) == b_cnt;
        // Byte-deterministic materialization: u1's dump == u2's dump.
        auto dump2 = r2.get_bin("lut");
        dq = dump1 && dump2 && dump1->size() == dump2->size() &&
             dump1->size() > 9 && (*dump1)[0] == 'X' && (*dump1)[1] == 'L' &&
             std::memcmp(dump1->data(), dump2->data(), dump1->size()) == 0;
    }

    // ---- 3. stale lease: unique throwaway build evicts LAST tick's (LRU) ----
    int stale = (seq == 0) ? -1 : 0;   // -1 = skipped (nothing to go stale yet)
    {
        xi::ScriptPackBuilder b3;
        bool ok3 = b3.valid();
        ok3 = b3.add_mp("bkeys", i64_array({1000 + seq})) && ok3;
        ok3 = b3.add_mp("bvals", i64_array({2000 + seq})) && ok3;
        auto p3 = b3.seal();
        auto r3 = xi::use("u1").process(p3);
        flt = flt || !ok3 || r3.is_fault() ||
              r3.get_i64("build_rc").value_or(-99) != 0 ||
              r3.get_i64("built").value_or(-1) != 1;   // unique content: real build
        auto th = r3.get_mp("handle");

        if (seq > 0 && !t_prev.empty()) {
            xi::ScriptPackBuilder b4;
            bool ok4 = b4.valid();
            ok4 = b4.add_mp("handle", t_prev.data(), t_prev.size()) && ok4;
            ok4 = b4.add_mp("qkeys", i64_array({1})) && ok4;
            auto p4 = b4.seal();
            auto r4 = xi::use("u2").process(p4);
            // The funnel rc stays 0 — staleness is a CONTRACT answer, a clean
            // sealed $fault pack from the type owner.
            stale = (ok4 && r4.get_i64("query_rc").value_or(-99) == 0 &&
                     r4.get_str("q_fault").value_or("") == "stale_handle") ? 1 : 0;
        }
        t_prev = th ? std::vector<uint8_t>(th->begin(), th->end())
                    : std::vector<uint8_t>{};
    }

    // ---- 4. wrong type: a foreign namespace's handle is refused -------------
    int wt = 0;
    {
        xi::mp::Writer w;
        w.map(4);
        w.key("type"); w.str("demo.image");
        w.key("id");   w.int_(0);
        w.key("gen");  w.int_(1);
        w.key("$v");   w.int_(1);
        xi::ScriptPackBuilder b5;
        bool ok5 = b5.valid();
        ok5 = b5.add_mp("handle", w) && ok5;
        ok5 = b5.add_mp("qkeys", i64_array({1})) && ok5;
        auto p5 = b5.seal();
        auto r5 = xi::use("u2").process(p5);
        wt = (ok5 && r5.get_i64("query_rc").value_or(-99) == 0 &&
              r5.get_str("q_fault").value_or("") == "wrong_type") ? 1 : 0;
    }

    const bool pass = built_ok && !flt && rc1_ok && clean1 && hop &&
                      zr1 && q1 && q2 && zr2 && dq && stale != 0 && wt == 1;

    char msg[224];
    std::snprintf(msg, sizeof msg,
                  "rhqa seq=%lld ok=%d flt=%d built=%lld b=%lld hop=%d q1=%d "
                  "q2=%d zr=%d dq=%d stale=%d wt=%d",
                  seq, (built_ok && rc1_ok && clean1) ? 1 : 0, flt ? 1 : 0,
                  built, b_cnt, hop ? 1 : 0, q1 ? 1 : 0, q2 ? 1 : 0,
                  (zr1 && zr2) ? 1 : 0, dq ? 1 : 0, stale, wt);
    if (pass) xi::ok(1, msg);
    else      xi::ng(1, msg);
}
