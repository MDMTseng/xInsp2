//
// hue_counter.cpp — count saturated, hue-banded blobs in a 3-channel image.
//
// process(input):
//   input image "src"  : 3-channel image (RGB; xi::imread → stb_image → RGB).
//   per-frame overrides: input["hue_lo"|"hue_hi"|"min_area"] (rare).
//
//   returns:
//     image "mask"   : 1-channel binary mask (0 / 255)
//     "count"        : int — connected components with area >= min_area
//     "hue_lo"       : int — band lower bound used this run
//     "hue_hi"       : int — band upper bound used this run
//     "min_area"     : int — area floor used this run
//
// exchange(cmd_json):
//   {"command":"set_hue_range","lo":int,"hi":int}
//   {"command":"set_min_area","value":int}
//   {"command":"reset"}
//   returns the new get_def() JSON.
//
// HSV details (OpenCV):
//   - Hue range is 0..179 (8-bit). Red sits near 0, green near 60, blue near 120.
//   - This plugin treats [hue_lo, hue_hi] as a simple inclusive range. It
//     does NOT wrap; if you need the wrapping red band (e.g. 170..10) you'd
//     run two ranges and OR the masks. Not needed for the hue_tune test.
//

#include <xi/xi_json.hpp>
#include <algorithm>
#include <cstdint>

class HueCounter : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        const xi::Image& src = input.get_image("src");
        if (src.empty() || src.channels != 3) {
            return xi::Record().set("error",
                std::string("hue_counter: need 3-channel 'src' image"));
        }

        // Per-frame overrides; default to stored config.
        int hlo  = input["hue_lo"].as_int(hue_lo_);
        int hhi  = input["hue_hi"].as_int(hue_hi_);
        int mina = input["min_area"].as_int(min_area_);
        hlo  = clamp_int(hlo,  0, 179);
        hhi  = clamp_int(hhi,  0, 179);
        mina = clamp_int(mina, 1, 1000000);
        if (hhi < hlo) std::swap(hlo, hhi);

        // RGB → HSV. xi::imread uses host's stb_image, which yields RGB.
        cv::Mat hsv;
        cv::cvtColor(src.as_cv_mat(), hsv, cv::COLOR_RGB2HSV);

        // mask = (H in [hlo,hhi]) AND (S >= S_FLOOR) AND (V >= V_FLOOR).
        xi::Image mask = pool_image(src.width, src.height, 1);
        cv::Scalar lo_s(hlo,  S_FLOOR, V_FLOOR);
        cv::Scalar hi_s(hhi,  255,     255);
        cv::inRange(hsv, lo_s, hi_s, mask.as_cv_mat());

        // Connected components (8-connected). stats[i,4] = area; label 0 is bg.
        cv::Mat labels, stats, centroids;
        int n_labels = cv::connectedComponentsWithStats(
            mask.as_cv_mat(), labels, stats, centroids, 8, CV_32S);

        int n_count = 0;
        for (int i = 1; i < n_labels; ++i) {
            int area = stats.at<int>(i, cv::CC_STAT_AREA);
            if (area >= mina) ++n_count;
        }

        last_count_ = n_count;
        ++frames_processed_;

        return xi::Record()
            .image("mask", mask)
            .set("count",    n_count)
            .set("hue_lo",   hlo)
            .set("hue_hi",   hhi)
            .set("min_area", mina);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "set_hue_range") {
            int lo = clamp_int(p["lo"].as_int(hue_lo_), 0, 179);
            int hi = clamp_int(p["hi"].as_int(hue_hi_), 0, 179);
            if (hi < lo) std::swap(lo, hi);
            hue_lo_ = lo;
            hue_hi_ = hi;
        } else if (command == "set_min_area") {
            min_area_ = clamp_int(p["value"].as_int(min_area_), 1, 1000000);
        } else if (command == "reset") {
            hue_lo_   = 0;
            hue_hi_   = 15;
            min_area_ = 300;
        }
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("hue_lo",           hue_lo_)
            .set("hue_hi",           hue_hi_)
            .set("min_area",         min_area_)
            .set("frames_processed", frames_processed_)
            .set("last_count",       last_count_)
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        hue_lo_   = clamp_int(p["hue_lo"].as_int(hue_lo_),     0, 179);
        hue_hi_   = clamp_int(p["hue_hi"].as_int(hue_hi_),     0, 179);
        min_area_ = clamp_int(p["min_area"].as_int(min_area_), 1, 1000000);
        if (hue_hi_ < hue_lo_) std::swap(hue_lo_, hue_hi_);
        return true;
    }

private:
    static constexpr int S_FLOOR = 80;
    static constexpr int V_FLOOR = 80;

    int hue_lo_   = 0;
    int hue_hi_   = 15;
    int min_area_ = 300;
    int frames_processed_ = 0;
    int last_count_ = 0;

    static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
};

XI_PLUGIN_IMPL(HueCounter)
