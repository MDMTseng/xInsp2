#pragma once
//
// io.hpp — extractor/constructor for synthetic_features.
//
// Shows the wiring layer handling a RICH contract: nested records, a typed
// array, and a plugin-author-defined nominal type (Feature) — all over the
// generic schema-less Record. Everything is total (NA-safe). See
// docs/design/io-types-and-na.md.
//
#include <xi/xi_record.hpp>
#include <xi/xi_types.hpp>

#include <vector>

namespace synth_io {

// --- a custom nominal type (plugins/toolboxes define their own) ------------
// Just a name over a Record, like the core xi:: types. A feature bundles a
// pose, a score, and an edge line.
class Feature : public xi::Typed {
public:
    XI_NOMINAL(Feature)
    xi::Pose pose()  const { return is_na() ? xi::Pose::na(na_reason()) : xi::Pose(record().get_record("pose")); }
    double   score() const { return num("score"); }
    xi::Line edge()  const { return is_na() ? xi::Line::na(na_reason()) : xi::Line(record().get_record("edge")); }
};

// --- output extractor ------------------------------------------------------
class Extract {
public:
    explicit Extract(xi::Record rec) : rec_(std::move(rec)) {}
    bool is_na() const { return rec_.is_na(); }

    // Scalars come back as a nominal xi::Number too (carrying src), so the WHOLE
    // output is record-class — feedable straight into another constructor.
    xi::Number count()      const { return numbered("count"); }
    xi::Number mean_score() const { return numbered("summary.mean_score"); }
    xi::Number best_score() const { return numbered("best.score"); }
    xi::Point  centroid()   const { return typed<xi::Point>("centroid"); }
    xi::Roi    roi()        const { return typed<xi::Roi>("roi"); }
    xi::Pose   best_pose()  const {
        if (rec_.is_na()) return xi::Pose::na(rec_.na_reason());
        xi::Pose p(rec_.get_record("best").get_record("pose"));
        p.set_src(rec_.src());
        return p;
    }

    int feature_count() const { return rec_.get_array_size("features"); }
    Feature feature(int i) const {
        if (rec_.is_na()) return Feature::na(rec_.na_reason());
        Feature f(rec_.get_array_item("features", i));
        f.set_src(rec_.src());
        return f;
    }
    std::vector<Feature> features() const {
        std::vector<Feature> out;
        const int n = feature_count();
        out.reserve(n);
        for (int i = 0; i < n; ++i) out.push_back(feature(i));
        return out;
    }

private:
    template <class T> T typed(const char* key) const {
        if (rec_.is_na()) return T::na(rec_.na_reason());
        T t(rec_.get_record(key));
        t.set_src(rec_.src());
        return t;
    }
    xi::Number numbered(const char* key) const {
        if (rec_.is_na()) return xi::Number::na(rec_.na_reason());
        xi::Number n(rec_[key].as_double(0));
        n.set_src(rec_.src());
        return n;
    }
    xi::Record rec_;
};
inline Extract extract(const xi::Record& rec) { return Extract(rec); }

// --- input constructor -----------------------------------------------------
class Build {
public:
    // Primary API: take the record-class values extract produces (they carry
    // their src, which the constructor records as provenance).
    Build& seed(const xi::Number& n)      { rec_.set("seed", (int)n.value());   rec_.set_prov("seed", n.src());      return *this; }
    Build& threshold(const xi::Number& n) { rec_.set("threshold", n.value());   rec_.set_prov("threshold", n.src()); return *this; }
    Build& roi(const xi::Roi& r)          { rec_.set("roi", r.record());        rec_.set_prov("roi", r.src());       return *this; }
    // Convenience for literal seeds (first stage, no upstream source).
    Build& seed(int s)         { rec_.set("seed", s); return *this; }
    Build& threshold(double t) { rec_.set("threshold", t); return *this; }
    xi::Record build() const { return rec_; }
private:
    xi::Record rec_;
};
inline Build build() { return Build(); }

} // namespace synth_io
