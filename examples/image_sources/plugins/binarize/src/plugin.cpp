//
// binarize.cpp — a tiny processing plugin: threshold an image to black/white.
//
// process(input "frame") -> { image "binary", "fg_pct", "threshold" }.
//
// The threshold is a per-instance param tuned from the webui slider. Changing it
// is plugin-INTERNAL: binarize re-thresholds its LAST input immediately and
// updates its own preview, so the webui shows the new result without a full-chain
// pass. get_def also returns input+output thumbnails so the webui can show a
// side-by-side input/output comparison.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_cv.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_image.hpp>   // xi::Image, xi::from_cv_mat

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {
std::string base64(const std::vector<uint8_t>& d) {
    static const char* a =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((d.size() + 2) / 3 * 4);
    for (size_t i = 0; i < d.size(); i += 3) {
        unsigned n = d[i] << 16;
        if (i + 1 < d.size()) n |= d[i + 1] << 8;
        if (i + 2 < d.size()) n |= d[i + 2];
        o.push_back(a[(n >> 18) & 63]);
        o.push_back(a[(n >> 12) & 63]);
        o.push_back(i + 1 < d.size() ? a[(n >> 6) & 63] : '=');
        o.push_back(i + 2 < d.size() ? a[n & 63] : '=');
    }
    return o;
}
// Small JPEG thumbnail (data: URI) of an xi::Image. Empty on failure.
std::string thumb(const xi::Image& img) {
    if (img.empty()) return "";
    const int TW = 200;
    cv::Mat src = xi::as_cv_read(img), small;   // reading the input -> read view
    if (img.width > TW) {
        int th = std::max(1, img.height * TW / img.width);
        cv::resize(src, small, cv::Size(TW, th), 0, 0, cv::INTER_AREA);
    } else src.copyTo(small);
    cv::Mat bgr;
    if (small.channels() == 3) cv::cvtColor(small, bgr, cv::COLOR_RGB2BGR);
    else                       bgr = small;   // 1-ch passes straight through
    std::vector<uint8_t> jpeg;
    std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 70 };
    if (cv::imencode(".jpg", bgr, jpeg, params))
        return "data:image/jpeg;base64," + base64(jpeg);
    return "";
}
} // namespace

class Binarize : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        xi::Image img = input.has_image("frame") ? input.get_image("frame") : first_(input);
        if (img.empty()) return xi::Record().set("ok", false);
        // Keep an OWNED copy so we can re-threshold on a slider change later.
        last_input_ = xi::Image(img.width, img.height, img.channels, img.data());
        recompute_();
        ++processed_;
        return xi::Record()
            .image("binary", last_binary_)
            .set("fg_pct", last_fg_)
            .set("threshold", threshold_)
            .set("ok", true);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "set_threshold") {
            threshold_ = std::clamp(p["value"].as_int(threshold_), 0, 255);
            recompute_();   // re-threshold the cached input -> live preview update
        }
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("threshold", threshold_)
            .set("last_fg_pct", last_fg_)
            .set("processed", processed_)
            .set("input_thumb", thumb(last_input_))
            .set("output_thumb", thumb(last_binary_))
            .dump();
    }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        threshold_ = std::clamp(p["threshold"].as_int(threshold_), 0, 255);
        recompute_();
        return true;
    }

private:
    int       threshold_ = 128;
    double    last_fg_    = 0.0;
    int       processed_  = 0;
    xi::Image last_input_;     // owned copy of the most recent input
    xi::Image last_binary_;    // its thresholded result (current threshold)

    // Threshold last_input_ -> last_binary_ + fg%. Re-run on every input AND on a
    // threshold change, so the webui preview reflects the current threshold.
    void recompute_() {
        if (last_input_.empty()) { last_binary_ = {}; last_fg_ = 0.0; return; }
        cv::Mat src = xi::as_cv_read(last_input_), gray;   // read the cached input
        if (src.channels() == 3)      cv::cvtColor(src, gray, cv::COLOR_RGB2GRAY);
        else if (src.channels() == 4) cv::cvtColor(src, gray, cv::COLOR_RGBA2GRAY);
        else                          gray = src;
        cv::Mat bin;
        cv::threshold(gray, bin, (double)threshold_, 255.0, cv::THRESH_BINARY);
        last_fg_ = (double)cv::countNonZero(bin) / std::max(1, bin.rows * bin.cols);
        last_binary_ = xi::from_cv_mat(bin);
    }

    xi::Image first_(const xi::Record& r) {
        for (auto& [k, im] : r.images()) if (!im.empty()) return im;
        return {};
    }
};

XI_PLUGIN_IMPL(Binarize)
