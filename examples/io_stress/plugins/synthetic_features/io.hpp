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
    xi::Pose pose()  const { return subview<xi::Pose>("pose"); }   // VIEW — no copy
    double   score() const { return num("score"); }
    xi::Line edge()  const { return subview<xi::Line>("edge"); }   // VIEW — no copy
};

// --- output extractor (SHALLOW) --------------------------------------------
// Shares the source Record (one shared_ptr) and hands out nominal VIEWS into its
// cJSON tree — no per-field deep copies. The only copy is the move of the
// (usually temporary) process() result into the shared root.
class Extract {
public:
    explicit Extract(std::shared_ptr<xi::Record> root) : root_(std::move(root)) {}
    bool is_na() const { return root_->is_na(); }

    // Scalars come back as a nominal xi::Number too (carrying src), so the WHOLE
    // output is record-class — feedable straight into another constructor.
    xi::Number count()      const { return numbered("count"); }
    xi::Number mean_score() const { return numbered("summary.mean_score"); }
    xi::Number best_score() const { return numbered("best.score"); }
    xi::Point  centroid()   const { return view<xi::Point>("centroid"); }
    xi::Roi    roi()        const { return view<xi::Roi>("roi"); }
    xi::Pose   best_pose()  const { return view<xi::Pose>("best.pose"); }

    int feature_count() const { return root_->get_array_size("features"); }
    Feature feature(int i) const {
        if (root_->is_na()) return Feature::na(root_->na_reason());
        cJSON* arr  = root_->at("features").raw();
        cJSON* node = arr ? cJSON_GetArrayItem(arr, i) : nullptr;
        if (!node) return Feature::na("synth_io: no feature at index");
        Feature f(root_, node);   // VIEW — no copy
        f.set_src(root_->src());
        return f;
    }
    std::vector<Feature> features() const {
        std::vector<Feature> out;
        cJSON* arr = root_->at("features").raw();
        const int n = arr ? cJSON_GetArraySize(arr) : 0;
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            Feature f(root_, cJSON_GetArrayItem(arr, i));
            f.set_src(root_->src());
            out.push_back(std::move(f));
        }
        return out;
    }

private:
    template <class T> T view(const char* path) const {
        if (root_->is_na()) return T::na(root_->na_reason());
        cJSON* node = root_->at(path).raw();
        if (!node) return T::na("synth_io: missing field");
        T t(root_, node);          // VIEW — shares root, points at the sub-node
        t.set_src(root_->src());
        return t;
    }
    xi::Number numbered(const char* key) const {
        if (root_->is_na()) return xi::Number::na(root_->na_reason());
        xi::Number n((*root_)[key].as_double(0));   // small scalar — kept owned
        n.set_src(root_->src());
        return n;
    }
    std::shared_ptr<xi::Record> root_;
};
inline Extract extract(xi::Record rec) {
    return Extract(std::make_shared<xi::Record>(std::move(rec)));
}

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
