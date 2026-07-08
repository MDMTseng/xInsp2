#pragma once
//
// xi_pack_report.hpp — the report-envelope convention helper (doc 27).
//
// Producers SHOULD emit one nested-msgpack entry `report = { status, data }`
// so official tooling (VSCode extension, expose, record_save, dashboards) reads
// per-instance status + results without knowing the producer's schema. This
// header makes the convention the easy path and enforces its two invariants by
// construction:
//
//   * status fields are exactly {state, value, msg, elapsed_us} (doc 27 §1);
//   * a fault is stamped on BOTH planes — the AUTHORITATIVE top-level $fault
//     (which the host funnel short-circuits/propagates on, doc 15) AND the
//     report.status MIRROR — so a plugin using ReportOut cannot hide a fault
//     inside `report` where the host would never see it (doc 27 rule 2).
//
// Images and $-reserved keys stay TOP-LEVEL (never inside report): write them on
// the PackOut directly, as before.
//
//   xi::ReportOut rep(out);
//   { auto d = rep.data(); d.add("count", (int64_t)n).add("score", score); d.close(); }
//   if (bad_roi) rep.fault("wrong_type", "roi", "expected i64", us);   // dual-stamps
//   else         rep.ok(1, "clean", us);
//   rep.finish();                       // emits report; must be called once
//   out.image("img0", w, h, c, px);     // images stay top-level
//
#include <xi/xi_abi.hpp>
#include <xi/xi_mp_build.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace xi {

// ---- status vocabulary (doc 27 §1, run-outcome class strings) --------------
// One home for the report.status `state` strings + a typed enum, so the value
// is not re-typed at each call site (the drift doc 27 warns about). Aligned with
// service_result.cpp outcome_class_for_code.
namespace report {
inline constexpr const char* kOk    = "ok";
inline constexpr const char* kNg    = "ng";
inline constexpr const char* kNa    = "na";
inline constexpr const char* kFault = "fault";

enum class State { unknown, ok, ng, na, fault };

inline State state_from(std::string_view s) {
    if (s == kOk)    return State::ok;
    if (s == kNg)    return State::ng;
    if (s == kNa)    return State::na;
    if (s == kFault) return State::fault;
    return State::unknown;
}
inline const char* state_str(State s) {
    switch (s) {
        case State::ok:    return kOk;
        case State::ng:    return kNg;
        case State::na:    return kNa;
        case State::fault: return kFault;
        default:           return "unknown";
    }
}
} // namespace report

// ---- WRITE ----------------------------------------------------------------

class ReportOut {
public:
    explicit ReportOut(PackOut& out) : out_(&out), report_(doc_.root_map()) {}

    // The `data` sub-map: append your process results (scalars / nested
    // maps+arrays via the returned builder), then close() it BEFORE stamping a
    // status. Call at most once.
    mp::MapBuilder data() { return report_.open_map("data"); }

    // Status stamps (call one, last). value = ok-n / ng-n index; us < 0 = unset.
    ReportOut& ok(int value = 1, std::string_view msg = "", int64_t us = -1) {
        write_status_(report::kOk, value, msg, us); return *this;
    }
    ReportOut& ng(int value, std::string_view msg = "", int64_t us = -1) {
        write_status_(report::kNg, value, msg, us); return *this;
    }
    // fault dual-stamps: authoritative top-level $fault (host short-circuit) +
    // the report.status mirror. `detail` (or the reason code if empty) rides msg.
    ReportOut& fault(const char* code, const char* key,
                     std::string_view detail = {}, int64_t us = -1) {
        out_->fault(code, key, detail);
        write_status_(report::kFault, 0, detail.empty() ? std::string_view(code ? code : "fault") : detail, us);
        return *this;
    }

    // Close the report map and splice it into the pack as `report`. Idempotent.
    void finish() {
        if (finished_) return;
        report_.close();
        out_->mp("report", doc_.bytes().data(), doc_.bytes().size());
        finished_ = true;
    }

private:
    void write_status_(std::string_view state, int value, std::string_view msg, int64_t us) {
        auto s = report_.open_map("status");
        s.add("state", state).add("value", (int64_t)value).add("msg", msg);
        if (us >= 0) s.add("elapsed_us", us);
        s.close();
    }
    PackOut*      out_;
    mp::Doc       doc_;
    mp::MapBuilder report_;
    bool          finished_ = false;
};

// ---- READ -----------------------------------------------------------------

// The status sub-map, with the doc-27 named fields. Borrowed view — valid only
// while the underlying PackIn handle is.
class StatusView {
public:
    StatusView() = default;
    explicit StatusView(mp::MapReader m) : m_(m) {}

    bool valid() const { return m_.valid(); }
    std::optional<std::string_view> state()   const { return m_.str("state"); }
    std::optional<int64_t>          value()   const { return m_.i64("value"); }
    std::optional<std::string_view> msg()     const { return m_.str("msg"); }
    std::optional<int64_t>          elapsed_us() const { return m_.i64("elapsed_us"); }
    report::State state_enum() const { return report::state_from(state().value_or("")); }
    bool is_fault() const { return state_enum() == report::State::fault; }

    mp::MapReader raw() const { return m_; }

private:
    mp::MapReader m_;
};

// Read the report envelope off an input pack. `report`/`status`/`data` are all
// nullopt-safe: a pack without the convention yields invalid views, so tooling
// degrades gracefully (doc 27 rule 1). The authoritative fault signal remains
// PackIn::is_fault() (top-level $fault); status().is_fault() is the mirror.
class ReportIn {
public:
    explicit ReportIn(const PackIn& in) : in_(in) {
        if (auto b = in.mp("report")) report_ = mp::MapReader(b->first, (size_t)b->second);
    }

    bool has_report() const { return report_.valid(); }
    StatusView    status() const { return StatusView(report_.map("status")); }
    mp::MapReader data()   const { return report_.map("data"); }
    mp::MapReader report() const { return report_; }

    // Path access over the whole envelope, e.g.
    //   rep.get<std::string_view>(".status.state", "")
    //   rep.get<int64_t>(".data.objects[3].x", -1)
    //   rep.getNode(".data.objects").asArray()
    mp::Node node() const { return report_.node(); }
    mp::Node getNode(std::string_view path) const { return report_.getNode(path); }
    template <class T> T get(std::string_view path, T fallback = T{}) const {
        return report_.template get<T>(path, fallback);
    }

    // Top-level authority (doc 15) — prefer this over status().is_fault().
    bool is_fault() const { return in_.is_fault(); }

private:
    PackIn        in_;
    mp::MapReader report_;
};

} // namespace xi
