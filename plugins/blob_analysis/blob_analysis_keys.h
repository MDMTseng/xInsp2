#pragma once
//
// blob_analysis_keys.h — the single source of truth for blob_analysis's
// Record key names (guard 1 of the plugin data contract).
//
// Every string key blob_analysis reads from its input Record or writes to its
// output Record is named EXACTLY ONCE here. The plugin's own process() reader,
// the Input builder, and the Output extractor (blob_analysis_io.h) all compile
// from these constants — so renaming a key is a one-line edit that can't drift
// between the wrapper header and the implementation.
//
// Bump kSchemaVersion whenever the input contract changes incompatibly (a
// required key added/renamed/retyped). The Input builder stamps this version
// into the Record; the plugin rejects a mismatched build with a precise error.
//

namespace xi::blob {

// Contract schema version. Bump on an incompatible INPUT change.
inline constexpr int kSchemaVersion = 1;

namespace keys {

// --- Inputs ---
inline constexpr const char* kGray      = "gray";       // image  (required, 1-channel)
inline constexpr const char* kThreshold = "threshold";  // int    (optional, default 128)
inline constexpr const char* kMinArea   = "min_area";   // int    (optional, default 10)
inline constexpr const char* kMaxArea   = "max_area";   // int    (optional, default 999999)
inline constexpr const char* kInvert    = "invert";     // bool   (optional, default false)

// --- Outputs ---
inline constexpr const char* kBinary        = "binary";         // image (thresholded)
inline constexpr const char* kBlobCount     = "blob_count";     // int
inline constexpr const char* kThresholdUsed = "threshold_used"; // int
inline constexpr const char* kBlobs         = "blobs";          // array of blob objects

// --- Fields inside each element of the "blobs" array ---
inline constexpr const char* kArea          = "area";           // int
inline constexpr const char* kCx            = "cx";             // double (centroid x)
inline constexpr const char* kCy            = "cy";             // double (centroid y)
inline constexpr const char* kMinX          = "min_x";          // int (bounding box)
inline constexpr const char* kMinY          = "min_y";          // int
inline constexpr const char* kMaxX          = "max_x";          // int
inline constexpr const char* kMaxY          = "max_y";          // int
inline constexpr const char* kContourPoints = "contour_points"; // int (count)
inline constexpr const char* kContour       = "contour";        // array of {x,y}
inline constexpr const char* kX             = "x";              // int (contour point)
inline constexpr const char* kY             = "y";              // int

}  // namespace keys
}  // namespace xi::blob
