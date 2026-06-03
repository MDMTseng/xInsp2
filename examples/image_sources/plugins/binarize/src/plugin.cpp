//
// binarize.cpp — a tiny processing plugin: threshold an image to black/white.
//
// process(input "frame") -> { image "binary", "fg_pct", "threshold" }.
// The threshold is a per-instance param tuned from the webui (a slider). Changing
// it is plugin-INTERNAL (no full-chain pass) — the new value takes effect on the
// next pass, which you trigger by replaying a cached frame (cached_image_source)
// or issuing a file (local_image_source). That's the tune-and-replay loop.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_image.hpp>   // xi::Image, xi::from_cv_mat

#include <algorithm>
#include <string>

class Binarize : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        xi::Image img = input.has_image("frame") ? input.get_image("frame") : first_(input);
        if (img.empty()) return xi::Record().set("ok", false);

        cv::Mat src = img.as_cv_mat(), gray;
        if (src.channels() == 3)      cv::cvtColor(src, gray, cv::COLOR_RGB2GRAY);
        else if (src.channels() == 4) cv::cvtColor(src, gray, cv::COLOR_RGBA2GRAY);
        else                          gray = src;

        cv::Mat bin;
        cv::threshold(gray, bin, (double)threshold_, 255.0, cv::THRESH_BINARY);
        double fg = (double)cv::countNonZero(bin) / std::max(1, bin.rows * bin.cols);
        last_fg_ = fg;
        ++processed_;

        return xi::Record()
            .image("binary", xi::from_cv_mat(bin))   // owned copy, safe to return
            .set("fg_pct", fg)
            .set("threshold", threshold_)
            .set("ok", true);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "set_threshold")
            threshold_ = std::clamp(p["value"].as_int(threshold_), 0, 255);
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("threshold", threshold_)
            .set("last_fg_pct", last_fg_)
            .set("processed", processed_)
            .dump();
    }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        threshold_ = std::clamp(p["threshold"].as_int(threshold_), 0, 255);
        return true;
    }

private:
    int    threshold_ = 128;
    double last_fg_   = 0.0;
    int    processed_ = 0;

    xi::Image first_(const xi::Record& r) {
        for (auto& [k, im] : r.images()) if (!im.empty()) return im;
        return {};
    }
};

XI_PLUGIN_IMPL(Binarize)
