#pragma once
//
// io.hpp — extractor helpers for blob_centroid_detector, used here as a LOCATOR.
//
// The plugin's raw output is schema-less cJSON (a "centroids" array of
// {x,y,area}). This extractor ADAPTS it into the typed vocabulary: each centroid
// becomes an xi::Pose (position only — angle 0). Total: never fails; an NA input
// (or a missing index) yields an NA Pose so the failure propagates.
//
// See docs/design/io-types-and-na.md.
//
#include <xi/xi_record.hpp>
#include <xi/xi_types.hpp>

#include <string>
#include <vector>

namespace blob_io {

class Extract {
public:
    explicit Extract(xi::Record rec) : rec_(std::move(rec)) {}

    bool is_na() const { return rec_.is_na(); }
    int  count() const { return rec_["count"].as_int(0); }
    int  orientation_count() const { return rec_["centroids"].size(); }

    // i-th located part as a Pose. Lazy: only this one is built.
    xi::Pose orientation(int i) const {
        if (rec_.is_na()) return xi::Pose::na(rec_.na_reason());
        auto c = rec_["centroids"][i];
        if (!c.exists()) return xi::Pose::na("blob_io: no orientation at index " + std::to_string(i));
        return xi::Pose(c["x"].as_double(0), c["y"].as_double(0), /*angle=*/0.0);
    }

    // All located parts. Cheap: xi::Pose is a lightweight handle.
    std::vector<xi::Pose> orientations() const {
        std::vector<xi::Pose> out;
        const int n = orientation_count();
        out.reserve(n);
        for (int i = 0; i < n; ++i) out.push_back(orientation(i));
        return out;
    }

private:
    xi::Record rec_;
};

inline Extract extract(const xi::Record& rec) { return Extract(rec); }

} // namespace blob_io
