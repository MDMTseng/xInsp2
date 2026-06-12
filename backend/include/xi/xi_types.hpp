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

// Base: a name over a shared Record. Derive and add domain accessors.
class Typed {
public:
    Typed() : rec_(std::make_shared<Record>()) {}
    explicit Typed(Record r) : rec_(std::make_shared<Record>(std::move(r))) {}

    // The schema-less payload — what crosses into a constructor / process().
    const Record& record() const { return *rec_; }
    Record&       record()       { return *rec_; }

    bool        is_na()     const { return rec_->is_na(); }
    std::string na_reason() const { return rec_->na_reason(); }

protected:
    // Schema-less field reads: missing / wrong-type / NA → the default.
    double      num(const char* k, double def = 0.0)            const { return (*rec_)[k].as_double(def); }
    int         i32(const char* k, int def = 0)                 const { return (*rec_)[k].as_int(def); }
    std::string str(const char* k, const std::string& d = "")   const { return (*rec_)[k].as_string(d); }

    std::shared_ptr<Record> rec_;
};

// Boilerplate every nominal type wants: default ctor, wrap-a-Record, typed NA.
#define XI_NOMINAL(Name)                                       \
    Name() = default;                                          \
    explicit Name(Record r) : Typed(std::move(r)) {}           \
    static Name na(const std::string& reason = "") {           \
        return Name(Record::na(reason));                       \
    }

// --- the common machine-vision vocabulary --------------------------------
// Field names are conventions (schema-less); accessors just read them.

class Number : public Typed {
public:
    XI_NOMINAL(Number)
    explicit Number(double v) { record().set("value", v); }
    double value(double def = 0.0) const { return num("value", def); }
};

class Point : public Typed {
public:
    XI_NOMINAL(Point)
    Point(double x, double y) { record().set("x", x).set("y", y); }
    double x() const { return num("x"); }
    double y() const { return num("y"); }
};

// Pose / orientation: position + angle (degrees, by convention).
class Pose : public Typed {
public:
    XI_NOMINAL(Pose)
    Pose(double x, double y, double angle) { record().set("x", x).set("y", y).set("angle", angle); }
    double x()     const { return num("x"); }
    double y()     const { return num("y"); }
    double angle() const { return num("angle"); }
};

class Line : public Typed {
public:
    XI_NOMINAL(Line)
    Line(double x1, double y1, double x2, double y2) {
        record().set("x1", x1).set("y1", y1).set("x2", x2).set("y2", y2);
    }
    double x1() const { return num("x1"); }  double y1() const { return num("y1"); }
    double x2() const { return num("x2"); }  double y2() const { return num("y2"); }
};

class Arc : public Typed {
public:
    XI_NOMINAL(Arc)
    Arc(double cx, double cy, double r, double a0, double a1) {
        record().set("cx", cx).set("cy", cy).set("r", r).set("a0", a0).set("a1", a1);
    }
    double cx() const { return num("cx"); }  double cy() const { return num("cy"); }
    double r()  const { return num("r");  }
    double a0() const { return num("a0"); }  double a1() const { return num("a1"); }
};

class Roi : public Typed {
public:
    XI_NOMINAL(Roi)
    Roi(double x, double y, double w, double h) {
        record().set("x", x).set("y", y).set("w", w).set("h", h);
    }
    double x() const { return num("x"); }  double y() const { return num("y"); }
    double w() const { return num("w"); }  double h() const { return num("h"); }
};

} // namespace xi
