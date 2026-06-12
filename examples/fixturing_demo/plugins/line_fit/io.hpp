#pragma once
//
// io.hpp — input constructor + output extractor for line_fit.
//
//   build():   one typed setter per input port — assembles the complete input
//              Record. Total: never fails; an incomplete build just yields a
//              Record the plugin will reject to NA.
//   extract(): one typed getter per output port. NA propagates into the wrapper.
//
// See docs/design/io-types-and-na.md.
//
#include <xi/xi_record.hpp>
#include <xi/xi_types.hpp>

namespace line_fit_io {

// --- input constructor ----------------------------------------------------
class Build {
public:
    Build& current(const xi::Pose& p) {
        rec_.set("current", p.record());
        rec_.set_prov("current", p.src());   // constructor records where this field came from
        return *this;
    }
    xi::Record build() const { return rec_; }
private:
    xi::Record rec_;
};
inline Build build() { return Build(); }

// --- output extractor -----------------------------------------------------
class Extract {
public:
    explicit Extract(xi::Record rec) : rec_(std::move(rec)) {}
    bool is_na() const { return rec_.is_na(); }

    xi::Line line() const {
        if (rec_.is_na()) return xi::Line::na(rec_.na_reason());   // NA flows into the typed result
        return xi::Line(rec_.get_record("line"));
    }
    xi::Pose current() const {
        if (rec_.is_na()) return xi::Pose::na(rec_.na_reason());
        return xi::Pose(rec_.get_record("current"));
    }

private:
    xi::Record rec_;
};
inline Extract extract(const xi::Record& rec) { return Extract(rec); }

} // namespace line_fit_io
