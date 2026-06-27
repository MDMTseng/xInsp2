#pragma once
//
// xi_types.hpp — nominal types for ergonomic, type-checked I/O wiring.
//
// Each type is JUST A NAME over a generic, schema-less xi::Record. No fields are
// enforced; the payload stays JSON (yyjson). The wrapper is a lightweight handle
// (shared payload — cheap to copy, so std::vector<Pose> is cheap), carries
// schema-less convenience accessors, and can be NA. The name is the vocabulary
// that extractors return, constructors accept, the manifest `kind` describes, and
// the (future) wiring UI uses to decide what connects to what. Types live ONLY in
// the wiring layer; the process() ABI stays untyped (Record).
//
// Plugin / toolbox authors define their own nominal types the same way (it's
// just source): derive from xi::Typed and add accessors. See
// docs/internals/typed-io.md.
//
#include "xi_record.hpp"

#include <memory>
#include <string>
#include <utility>

namespace xi {

// Write a double as JSON, but emit the quoted NaN/Infinity sentinel for a
// non-finite value (same guard as Record::set(double)). A bare NaN/Infinity
// token is invalid JSON and would make the consumer's parse of the WHOLE record
// fail — the typed layer must not bypass that protection.
inline yyjson_mut_val* mut_finite_(yyjson_mut_doc* d, double v) {
    return std::isfinite(v) ? yyjson_mut_real(d, v)
                            : yyjson_mut_strcpy(d, nonfinite_to_str(v));
}

// A read/write proxy for one field of a yyjson object node. Returned by
// Typed::operator[]. Reads via implicit conversion (so it drops into arithmetic),
// writes via operator= (write-through to the node). Lives only for the
// statement — do NOT `auto`-capture it (it aliases the node, like a
// std::vector<bool> reference). Usage:
//
//   roi["x"] = roi["x"] * 0.533 + 45;   // read-modify-write
//   double w = roi["w"];                 // read
//
class Field {
public:
    Field(yyjson_mut_doc* doc, yyjson_mut_val* node, const char* key)
        : doc_(doc), node_(node), key_(key) {}

    operator double() const { return Record::Value(node_)[key_.c_str()].as_double(0); }
    double      as_double(double d = 0)         const { return Record::Value(node_)[key_.c_str()].as_double(d); }
    int         as_int(int d = 0)               const { return Record::Value(node_)[key_.c_str()].as_int(d); }
    bool        as_bool(bool d = false)         const { return Record::Value(node_)[key_.c_str()].as_bool(d); }
    std::string as_string(const std::string& d = "") const { return Record::Value(node_)[key_.c_str()].as_string(d); }

    Field& operator=(double v)             { set_(mut_finite_(doc_, v));                     return *this; }
    Field& operator=(int v)                { set_(yyjson_mut_sint(doc_, v));                 return *this; }
    Field& operator=(bool v)               { set_(yyjson_mut_bool(doc_, v));                 return *this; }
    Field& operator=(const std::string& v) { set_(yyjson_mut_strcpy(doc_, v.c_str()));       return *this; }
    Field& operator=(const char* v)        { set_(yyjson_mut_strcpy(doc_, v ? v : ""));      return *this; }
    Field& operator=(const Field& o)       { return *this = static_cast<double>(o); }   // value copy

private:
    void set_(yyjson_mut_val* item) {
        if (!node_ || !item || !yyjson_mut_is_obj(node_)) return;
        yyjson_mut_obj_remove_key(node_, key_.c_str());
        yyjson_mut_obj_put(node_, yyjson_mut_strcpy(doc_, key_.c_str()), item);
    }
    yyjson_mut_doc* doc_;
    yyjson_mut_val* node_;
    std::string     key_;
};

// Base: a NAME over a Record, with a SHALLOW (view) mode.
//
//   - OWNED  : holds its own Record (root_), node_ == root_->json(). Field ctors
//              build into it.
//   - VIEW   : shares another value's root_ (refcount bump, no copy) and points
//              node_ at a sub-node of it. Extractors hand these out so pulling N
//              nested values / array items costs no duplication.
//
// record() materializes a standalone Record (copy of the viewed node) only when
// you embed the value into a constructor input — the one unavoidable copy.
class Typed {
public:
    Typed() : root_(std::make_shared<Record>()), node_(root_->json()) {}
    explicit Typed(Record r) : root_(std::make_shared<Record>(std::move(r))), node_(root_->json()) {}
    // VIEW ctor: share `root`, point at `node` (a yyjson val inside root's tree).
    Typed(std::shared_ptr<Record> root, yyjson_mut_val* node) : root_(std::move(root)), node_(node) {}

    // Standalone Record of this value — for embedding into a constructor input.
    // Owned: copy the root; view: deep-copy just the node.
    Record record() const {
        if (root_ && node_ == root_->json()) return *root_;
        return Record::Value(node_).as_record();
    }

    bool is_na() const {
        return node_ && yyjson_mut_is_obj(node_) && yyjson_mut_obj_get(node_, Record::kNaKey) != nullptr;
    }
    std::string na_reason() const {
        yyjson_mut_val* n = (node_ && yyjson_mut_is_obj(node_)) ? yyjson_mut_obj_get(node_, Record::kNaKey) : nullptr;
        const char* s = (n && yyjson_mut_is_str(n)) ? yyjson_mut_get_str(n) : nullptr;
        return s ? std::string(s) : "";
    }

    // Provenance: where this value came from. Kept on the wrapper (a view doesn't
    // own its node's tree, so it can't stamp $src into it). Extractors pipe the
    // producing instance's src on. See docs/internals/typed-io.md.
    std::string src()                  const { return src_; }
    Typed&      set_src(const std::string& id) { src_ = id; return *this; }

    // --- WRITE-THROUGH setters ---------------------------------------------
    // Writes the viewed node. A VIEW writes into the SHARED tree, so it mutates
    // the ORIGINAL it was extracted from — by design (like a NumPy view). If you
    // don't want that, .clone() first for an independent copy.
    Typed& set(const char* k, double v)             { set_node_(k, mut_finite_(mut_doc(), v)); return *this; }
    Typed& set(const char* k, int v)                { set_node_(k, yyjson_mut_sint(mut_doc(), v)); return *this; }
    Typed& set(const char* k, bool v)               { set_node_(k, yyjson_mut_bool(mut_doc(), v)); return *this; }
    Typed& set(const char* k, const std::string& v) { set_node_(k, yyjson_mut_strcpy(mut_doc(), v.c_str())); return *this; }

    // Field proxy: read (implicit) + write (operator=), so e.g.
    //   roi["x"] = roi["x"] * 0.533 + 45;
    Field operator[](const char* key) const { return Field(mut_doc(), node_, key); }

protected:
    yyjson_mut_doc* mut_doc() const { return root_ ? root_->doc() : nullptr; }

    // A nested value as a VIEW into this one's tree (no copy). `key` is a direct
    // child name. NA-safe. Used by composite types (e.g. Feature::pose()).
    template <class T> T subview(const char* key) const {
        if (is_na()) return T::na(na_reason());
        yyjson_mut_val* n = (node_ && yyjson_mut_is_obj(node_)) ? yyjson_mut_obj_get(node_, key) : nullptr;
        if (!n) return T::na("missing sub-field");
        T t(root_, n);
        t.set_src(src_);
        return t;
    }
    void set_node_(const char* k, yyjson_mut_val* item) {
        if (!node_ || !item || !yyjson_mut_is_obj(node_)) return;
        yyjson_mut_obj_remove_key(node_, k);
        yyjson_mut_obj_put(node_, yyjson_mut_strcpy(mut_doc(), k), item);
    }
    // Schema-less field reads off the viewed node: missing / wrong-type → default.
    double      num(const char* k, double def = 0.0)            const { return Record::Value(node_)[k].as_double(def); }
    int         i32(const char* k, int def = 0)                 const { return Record::Value(node_)[k].as_int(def); }
    std::string str(const char* k, const std::string& d = "")   const { return Record::Value(node_)[k].as_string(d); }

    std::shared_ptr<Record> root_;
    yyjson_mut_val*          node_ = nullptr;
    std::string             src_;
};

// Boilerplate every nominal type wants: default ctor, wrap-a-Record, typed NA.
// Fully qualified so plugin/toolbox authors can use it in their OWN namespace.
#define XI_NOMINAL(Name)                                                       \
    Name() = default;                                                          \
    explicit Name(::xi::Record r) : ::xi::Typed(std::move(r)) {}               \
    Name(std::shared_ptr<::xi::Record> root, ::yyjson_mut_val* node)           \
        : ::xi::Typed(std::move(root), node) {}                               \
    static Name na(const std::string& reason = "") {                          \
        return Name(::xi::Record::na(reason));                                 \
    }                                                                          \
    /* An independent OWNED copy — set() on it won't touch the original. */    \
    Name clone() const { return Name(this->record()); }

// --- the common machine-vision vocabulary --------------------------------
// Field names are conventions (schema-less); accessors just read them.

class Number : public Typed {
public:
    XI_NOMINAL(Number)
    explicit Number(double v) { set("value", v); }
    double value(double def = 0.0) const { return num("value", def); }
};

class Point : public Typed {
public:
    XI_NOMINAL(Point)
    Point(double x, double y) { set("x", x).set("y", y); }
    double x() const { return num("x"); }
    double y() const { return num("y"); }
};

// Pose / orientation: position + angle (degrees, by convention).
class Pose : public Typed {
public:
    XI_NOMINAL(Pose)
    Pose(double x, double y, double angle) { set("x", x).set("y", y).set("angle", angle); }
    double x()     const { return num("x"); }
    double y()     const { return num("y"); }
    double angle() const { return num("angle"); }
};

class Line : public Typed {
public:
    XI_NOMINAL(Line)
    Line(double x1, double y1, double x2, double y2) {
        set("x1", x1).set("y1", y1).set("x2", x2).set("y2", y2);
    }
    double x1() const { return num("x1"); }  double y1() const { return num("y1"); }
    double x2() const { return num("x2"); }  double y2() const { return num("y2"); }
};

class Arc : public Typed {
public:
    XI_NOMINAL(Arc)
    Arc(double cx, double cy, double r, double a0, double a1) {
        set("cx", cx).set("cy", cy).set("r", r).set("a0", a0).set("a1", a1);
    }
    double cx() const { return num("cx"); }  double cy() const { return num("cy"); }
    double r()  const { return num("r");  }
    double a0() const { return num("a0"); }  double a1() const { return num("a1"); }
};

class Roi : public Typed {
public:
    XI_NOMINAL(Roi)
    Roi(double x, double y, double w, double h) {
        set("x", x).set("y", y).set("w", w).set("h", h);
    }
    double x() const { return num("x"); }  double y() const { return num("y"); }
    double w() const { return num("w"); }  double h() const { return num("h"); }
};

// Small fixed vectors — {x,y}, {x,y,z}, {x,y,z,w}. Components are also
// reachable by name via operator[] (vec["y"] = ...).
class Vec2 : public Typed {
public:
    XI_NOMINAL(Vec2)
    Vec2(double x, double y) { set("x", x).set("y", y); }
    double x() const { return num("x"); }
    double y() const { return num("y"); }
};

class Vec3 : public Typed {
public:
    XI_NOMINAL(Vec3)
    Vec3(double x, double y, double z) { set("x", x).set("y", y).set("z", z); }
    double x() const { return num("x"); }
    double y() const { return num("y"); }
    double z() const { return num("z"); }
};

class Vec4 : public Typed {
public:
    XI_NOMINAL(Vec4)
    Vec4(double x, double y, double z, double w) { set("x", x).set("y", y).set("z", z).set("w", w); }
    double x() const { return num("x"); }
    double y() const { return num("y"); }
    double z() const { return num("z"); }
    double w() const { return num("w"); }
};

// N×N matrix — stored row-major as a flat array under "m". Defaults to identity.
// The point is to carry a matrix through the Record and bridge to cv::Matx for
// the actual math (xi_types_cv.hpp: to_cv / from_cv). at()/set() are element
// access; for real linear algebra go through OpenCV.
template <int N>
class MatN : public Typed {
public:
    MatN() { set_identity_(); }
    explicit MatN(Record r) : Typed(std::move(r)) {}
    MatN(std::shared_ptr<Record> root, yyjson_mut_val* node) : Typed(std::move(root), node) {}
    static MatN na(const std::string& reason = "") { return MatN(Record::na(reason)); }
    MatN clone() const { return MatN(this->record()); }

    static constexpr int dim = N;
    double at(int r, int c) const { return elem_(r * N + c); }
    MatN&  set(int r, int c, double v) {
        yyjson_mut_val* arr = (node_ && yyjson_mut_is_obj(node_)) ? yyjson_mut_obj_get(node_, "m") : nullptr;
        yyjson_mut_val* e   = arr ? yyjson_mut_arr_get(arr, (size_t)(r * N + c)) : nullptr;
        // Route through the non-finite sentinel like every other double write — a raw
        // NaN/Inf real makes yyjson_mut_write() fail, which silently drops the WHOLE
        // record's JSON on the wire (the hazard the sentinel exists to prevent). The
        // sentinel string is a static literal, so set_str (no-copy) is safe.
        if (e) {
            if (std::isfinite(v)) yyjson_mut_set_real(e, v);
            else                  yyjson_mut_set_str(e, nonfinite_to_str(v));
        }
        return *this;
    }

private:
    void set_data_(const double* d) {
        yyjson_mut_doc* doc = mut_doc();
        yyjson_mut_val* arr = yyjson_mut_arr(doc);
        for (int i = 0; i < N * N; ++i) yyjson_mut_arr_add_val(arr, mut_finite_(doc, d[i]));
        set_node_("m", arr);
    }
    void set_identity_() {
        double I[N * N] = {};
        for (int i = 0; i < N; ++i) I[i * N + i] = 1.0;
        set_data_(I);
    }
    double elem_(int idx) const {
        yyjson_mut_val* arr = (node_ && yyjson_mut_is_obj(node_)) ? yyjson_mut_obj_get(node_, "m") : nullptr;
        yyjson_mut_val* e   = arr ? yyjson_mut_arr_get(arr, (size_t)idx) : nullptr;
        if (!e) return 0.0;
        if (yyjson_mut_is_num(e)) return yyjson_mut_get_num(e);
        // Restore a non-finite sentinel string (written by set_data_/set) — else an
        // Inf/NaN element reads back 0.0 (silent wrong matrix value).
        if (yyjson_mut_is_str(e)) { double v; if (nonfinite_from_str(yyjson_mut_get_str(e), v)) return v; }
        return 0.0;
    }
};
using Mat2 = MatN<2>;
using Mat3 = MatN<3>;
using Mat4 = MatN<4>;

// --- iconic types (payload rides the IMAGE channel, not JSON) ------------
//
// Region is the first nominal type whose data is NOT JSON: it's a binary MASK
// image that travels on the Record's image channel (key "mask"), with only light
// metadata (frame w/h) mirrored into json for cheap reads. Because the bytes live
// in images_, a Region is zero-copy through the in-process ImagePool and goes
// straight to OpenCV via `mask().as_cv_mat()`.
//
// This is the BINARY case (CV_8U, 1-channel, nonzero = inside). OpenCV helpers
// (to_cv / region_from_mask / area / bbox) live in the opt-in xi_types_cv.hpp.
class Region : public Typed {
public:
    XI_NOMINAL(Region)

    // Wrap a binary mask. The mask rides the image channel; frame width/height
    // are mirrored into json so width()/height() don't have to touch pixels.
    explicit Region(Image mask) {
        const int w = mask.width, h = mask.height;
        if (root_) root_->image(kMaskKey, std::move(mask));
        set("w", w).set("h", h);
    }
    static constexpr const char* kMaskKey = "mask";

    // The binary mask. Empty Image if this Region is NA or carries no mask.
    Image mask() const { return root_ ? root_->get_image(kMaskKey) : Image{}; }
    int  width()  const { return i32("w"); }
    int  height() const { return i32("h"); }
    bool empty()  const { return mask().empty(); }
};

} // namespace xi
