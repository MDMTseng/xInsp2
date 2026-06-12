#pragma once
//
// xi_types.hpp — nominal types for ergonomic, type-checked I/O wiring.
//
// Each type is JUST A NAME over a generic, schema-less xi::Record. No fields are
// enforced; the payload stays cJSON. The wrapper is a lightweight handle (shared
// payload — cheap to copy, so std::vector<Pose> is cheap), carries schema-less
// convenience accessors, and can be NA. The name is the vocabulary that
// extractors return, constructors accept, the manifest `kind` describes, and the
// (future) wiring UI uses to decide what connects to what. Types live ONLY in
// the wiring layer; the process() ABI stays untyped (Record).
//
// Plugin / toolbox authors define their own nominal types the same way (it's
// just source): derive from xi::Typed and add accessors. See
// docs/design/io-types-and-na.md.
//
#include "xi_record.hpp"

#include <memory>
#include <string>
#include <utility>

namespace xi {

// Base: a NAME over a Record, with a SHALLOW (view) mode.
//
//   - OWNED  : holds its own Record (root_), node_ == root_->json(). Field ctors
//              build into it.
//   - VIEW   : shares another value's root_ (refcount bump, no copy) and points
//              node_ at a sub-node of it. Extractors hand these out so pulling N
//              nested values / array items costs no cJSON duplication.
//
// record() materializes a standalone Record (copy of the viewed node) only when
// you embed the value into a constructor input — the one unavoidable copy.
class Typed {
public:
    Typed() : root_(std::make_shared<Record>()), node_(root_->json()) {}
    explicit Typed(Record r) : root_(std::make_shared<Record>(std::move(r))), node_(root_->json()) {}
    // VIEW ctor: share `root`, point at `node` (a cJSON inside root's tree).
    Typed(std::shared_ptr<Record> root, cJSON* node) : root_(std::move(root)), node_(node) {}

    // Standalone Record of this value — for embedding into a constructor input.
    // Owned: copy the root; view: deep-copy just the node.
    Record record() const {
        if (root_ && node_ == root_->json()) return *root_;
        return Record::Value(node_).as_record();
    }

    bool        is_na()     const { return node_ && cJSON_GetObjectItem(node_, Record::kNaKey) != nullptr; }
    std::string na_reason() const {
        cJSON* n = node_ ? cJSON_GetObjectItem(node_, Record::kNaKey) : nullptr;
        return (n && cJSON_IsString(n)) ? n->valuestring : "";
    }

    // Provenance: where this value came from. Kept on the wrapper (a view doesn't
    // own its node's tree, so it can't stamp $src into it). Extractors pipe the
    // producing instance's src on. See docs/design/io-types-and-na.md.
    std::string src()                  const { return src_; }
    Typed&      set_src(const std::string& id) { src_ = id; return *this; }

    // --- WRITE-THROUGH setters ---------------------------------------------
    // Writes the viewed node. A VIEW writes into the SHARED tree, so it mutates
    // the ORIGINAL it was extracted from — by design (like a NumPy view). If you
    // don't want that, .clone() first for an independent copy. See
    // docs/design/io-types-and-na.md.
    Typed& set(const char* k, double v)             { set_node_(k, cJSON_CreateNumber(v)); return *this; }
    Typed& set(const char* k, int v)                { set_node_(k, cJSON_CreateNumber(v)); return *this; }
    Typed& set(const char* k, bool v)               { set_node_(k, cJSON_CreateBool(v));   return *this; }
    Typed& set(const char* k, const std::string& v) { set_node_(k, cJSON_CreateString(v.c_str())); return *this; }

protected:
    // A nested value as a VIEW into this one's tree (no copy). `key` is a direct
    // child name. NA-safe. Used by composite types (e.g. Feature::pose()).
    template <class T> T subview(const char* key) const {
        if (is_na()) return T::na(na_reason());
        cJSON* n = node_ ? cJSON_GetObjectItem(node_, key) : nullptr;
        if (!n) return T::na("missing sub-field");
        T t(root_, n);
        t.set_src(src_);
        return t;
    }
    void set_node_(const char* k, cJSON* item) {
        if (!node_ || !item) { if (item) cJSON_Delete(item); return; }
        cJSON_DeleteItemFromObject(node_, k);
        cJSON_AddItemToObject(node_, k, item);
    }
    // Schema-less field reads off the viewed node: missing / wrong-type → default.
    double      num(const char* k, double def = 0.0)            const { return Record::Value(node_)[k].as_double(def); }
    int         i32(const char* k, int def = 0)                 const { return Record::Value(node_)[k].as_int(def); }
    std::string str(const char* k, const std::string& d = "")   const { return Record::Value(node_)[k].as_string(d); }

    std::shared_ptr<Record> root_;
    cJSON*                   node_ = nullptr;
    std::string             src_;
};

// Boilerplate every nominal type wants: default ctor, wrap-a-Record, typed NA.
// Fully qualified so plugin/toolbox authors can use it in their OWN namespace.
#define XI_NOMINAL(Name)                                                       \
    Name() = default;                                                          \
    explicit Name(::xi::Record r) : ::xi::Typed(std::move(r)) {}               \
    Name(std::shared_ptr<::xi::Record> root, cJSON* node)                      \
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

} // namespace xi
