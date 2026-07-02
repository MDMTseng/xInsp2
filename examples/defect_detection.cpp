//
// defect_detection.cpp — example inspection using OpenCV directly.
//
// A self-contained OpenCV pipeline (gray → blur → threshold → morphology →
// connected components) on a synthetic frame generated inline. In a real
// project a source plugin would emit the frame and this script would read it
// via xi::current_trigger().image(...) (see examples/trigger_metadata) — here
// we generate one inline so the example needs no source.
//

#include <xi/xi.hpp>
#include <xi/xi_cv.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_use.hpp>       // xi::use("expose") — the data-out surface
#include <xi/xi_result.hpp>    // xi::ok / xi::ng — the per-run verdict

#include <vector>

// Tunable parameters — show up in the VS Code Params panel
static xi::Param<int>    blur_radius {"blur_radius",  2,   {0, 10}};
static xi::Param<int>    thresh_val  {"thresh_val",   180, {0, 255}};
static xi::Param<int>    morph_radius{"morph_radius", 1,   {0, 5}};

// Synthetic 320x240 RGB frame: a shifting gradient (varies with `seq` so a
// continuous run isn't a static image).
static xi::Image make_frame(int seq) {
    const int W = 320, H = 240;
    std::vector<uint8_t> px((size_t)W * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = ((size_t)y * W + x) * 3;
            px[i + 0] = (uint8_t)((x + seq * 3) & 0xFF);
            px[i + 1] = (uint8_t)((y + seq * 5) & 0xFF);
            px[i + 2] = (uint8_t)((x + y + seq) & 0xFF);
        }
    return xi::Image(W, H, 3, px.data());
}

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::Image rgb = make_frame(frame);
    if (rgb.empty()) return;

    cv::Mat src = xi::as_cv_mat(rgb);

    cv::Mat gray;
    cv::cvtColor(src, gray, cv::COLOR_RGB2GRAY);

    cv::Mat blurred;
    int br = (int)blur_radius;
    if (br > 0) {
        int k = 2 * br + 1;
        cv::GaussianBlur(gray, blurred, cv::Size(k, k), 0);
    } else {
        blurred = gray;
    }

    cv::Mat binary;
    cv::threshold(blurred, binary, (int)thresh_val, 255, cv::THRESH_BINARY);

    cv::Mat cleaned;
    int mr = (int)morph_radius;
    if (mr > 0) {
        int k = 2 * mr + 1;
        auto kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(k, k));
        cv::morphologyEx(binary, cleaned, cv::MORPH_OPEN, kernel);
    } else {
        cleaned = binary;
    }

    cv::Mat edges;
    cv::Sobel(gray, edges, CV_8U, 1, 1);

    cv::Mat labels;
    int n_labels = cv::connectedComponents(cleaned, labels, 8, CV_32S);
    int blob_count = std::max(0, n_labels - 1);

    cv::Scalar mean_scalar = cv::mean(gray);

    // Surface the pipeline stages + values through the `expose` plugin — the
    // current data-out surface (the old VAR image-preview path was removed).
    // Wrap each cv::Mat back as an xi::Image; each ctor does one heap copy. For
    // hot-path code, write cv:: output straight into a pool-backed Image via
    // Image::create_in_pool + as_cv_mat() instead (see the plugin examples).
    // Display order follows the record's key order, so the stages render
    // top-to-bottom as written.
    xi::use("expose").process(xi::Record()
        .set("$channel", "defect")
        .image("input",   rgb)
        .image("gray",    xi::Image(gray.cols,    gray.rows,    1, gray.data))
        .image("blurred", xi::Image(blurred.cols, blurred.rows, 1, blurred.data))
        .image("binary",  xi::Image(binary.cols,  binary.rows,  1, binary.data))
        .image("cleaned", xi::Image(cleaned.cols, cleaned.rows, 1, cleaned.data))
        .image("edges",   xi::Image(edges.cols,   edges.rows,   1, edges.data))
        .set("blob_count",     blob_count)
        .set("mean_intensity", mean_scalar[0]));

    // The run's pass/fail verdict rides its own dedicated path.
    if (blob_count <= 2) xi::ok(1, "clean");
    else                 xi::ng(1, "too many blobs");
}
