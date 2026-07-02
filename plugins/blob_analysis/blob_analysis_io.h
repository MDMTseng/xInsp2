#pragma once
//
// blob_analysis_io.h — the typed view over blob_analysis's Record I/O
// (the stage-1 deliverable of the plugin data contract).
//
// A script that drives blob_analysis includes THIS header and never touches a
// raw string key:
//
//   #include "blob_analysis_io.h"
//
//   xi::Record in = xi::blob::Input()
//       .gray(gray_img)
//       .threshold(90)
//       .min_area(25);
//   xi::blob::Output out{ xi::use("det0").process(in) };
//   if (!out.ok()) return xi::result::na(out.na_reason());   // fail-loud verdict
//   for (int i = 0; i < out.blob_count(); ++i)
//       total_area += out.blob(i).area();
//
// Both Input and Output compile straight down to xi::Record set/get over the
// blob_analysis_keys.h constants — NOTHING new crosses the ABI. A key typo
// becomes a compile error (there is no string to mistype), and the schema stamp
// the builder writes lets the plugin report a header/plugin skew precisely.
//
// Convention (guard 4 — identical in every plugin's *_io.h so stage-2 codegen
// can regenerate them):
//   * `Input`  — default-constructs an empty Record, stamps the schema, exposes
//                one chainable typed setter per input key, converts to
//                `const xi::Record&`.
//   * `Output` — wraps a returned Record by value, exposes `ok()`/`na_reason()`
//                plus one typed getter per output key.
//

#include "blob_analysis_keys.h"

#include <xi/xi_contract.hpp>
#include <xi/xi_record.hpp>

namespace xi::blob {

// ---- Input builder ---------------------------------------------------------
class Input {
public:
    Input() { rec_.set(xi::contract::kSchemaKey, kSchemaVersion); }

    Input& gray(xi::Image img)  { rec_.image(keys::kGray, std::move(img)); return *this; }
    Input& threshold(int v)     { rec_.set(keys::kThreshold, v); return *this; }
    Input& min_area(int v)      { rec_.set(keys::kMinArea, v);   return *this; }
    Input& max_area(int v)      { rec_.set(keys::kMaxArea, v);   return *this; }
    Input& invert(bool v)       { rec_.set(keys::kInvert, v);    return *this; }

    const xi::Record& record() const { return rec_; }
    operator const xi::Record&() const { return rec_; }

private:
    xi::Record rec_;
};

// ---- Output extractor ------------------------------------------------------
class Output {
public:
    explicit Output(xi::Record rec) : rec_(std::move(rec)) {}

    // Contract verdict. false when the plugin returned a structured failure
    // (missing input, schema skew, ...); na_reason()/fault_code() explain it.
    bool ok() const { return !rec_.is_na(); }
    std::string na_reason() const { return rec_.na_reason(); }
    std::string fault_code() const {
        return rec_.get_record(xi::contract::kFaultKey).get_string(xi::contract::kCode);
    }

    const xi::Image& binary() const { return rec_.get_image(keys::kBinary); }
    int blob_count() const     { return rec_.get_int(keys::kBlobCount); }
    int threshold_used() const { return rec_.get_int(keys::kThresholdUsed); }

    // A single blob (0 <= i < blob_count()).
    class Blob {
    public:
        explicit Blob(xi::Record r) : r_(std::move(r)) {}
        int    area()   const { return r_.get_int(keys::kArea); }
        double cx()     const { return r_.get_double(keys::kCx); }
        double cy()     const { return r_.get_double(keys::kCy); }
        int    min_x()  const { return r_.get_int(keys::kMinX); }
        int    min_y()  const { return r_.get_int(keys::kMinY); }
        int    max_x()  const { return r_.get_int(keys::kMaxX); }
        int    max_y()  const { return r_.get_int(keys::kMaxY); }
        int    contour_points() const { return r_.get_int(keys::kContourPoints); }
    private:
        xi::Record r_;
    };
    Blob blob(int i) const { return Blob(rec_.get_array_item(keys::kBlobs, i)); }

    const xi::Record& record() const { return rec_; }

private:
    xi::Record rec_;
};

}  // namespace xi::blob
