//
// golden_defect_finder.cpp — pixel-difference defect detector.
//
// Run-time plugin. Inputs: 'reference' (golden) + 'frame' (test). Both
// 1-channel uint8, same dims. Output: defect_present, bbox of largest
// blob, score, plus diff/mask images for visualisation.
//
// Pipeline:
//   diff   = |frame - reference|              // absolute difference
//   blur   = gaussian(diff, blur_radius)      // suppress per-pixel noise
//   mask   = blur > diff_threshold            // binary
//   mask   = close(mask, close_radius)        // join near pixels (scratches)
//   regions = findFilledRegions(mask)
//   keep   regions with min_area <= area <= max_area
//   defect_present = (kept regions count > 0)
//   bbox   = bounding box of LARGEST kept region
//   score  = largest_kept_area / min_area     (rough confidence)
//
// Tunable via UI. Per-run override via input record fields.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_ops.hpp>

#include <algorithm>
#include <cstdint>

class GoldenDefectFinder : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        const xi::Image& ref   = input.get_image("reference");
        const xi::Image& frame = input.get_image("frame");
        if (ref.empty() || frame.empty()) {
            return xi::Record().set("error",
                std::string("golden_defect_finder: need 'reference' and 'frame' images"));
        }
        if (ref.channels != 1 || frame.channels != 1) {
            return xi::Record().set("error",
                std::string("golden_defect_finder: both images must be 1-channel"));
        }
        if (ref.width != frame.width || ref.height != frame.height) {
            return xi::Record().set("error",
                std::string("golden_defect_finder: reference/frame size mismatch"));
        }

        // per-run override: input fields > stored config
        int blur_r  = input["blur_radius"].as_int(blur_radius_);
        int thresh  = input["diff_threshold"].as_int(diff_threshold_);
        int close_r = input["close_radius"].as_int(close_radius_);
        int min_a   = input["min_area"].as_int(min_area_);
        int max_a   = input["max_area"].as_int(max_area_);
        if (max_a < min_a) max_a = min_a;

        const int W = ref.width, H = ref.height, N = W * H;

        // 1) absolute difference
        xi::Image diff(W, H, 1);
        const uint8_t* rp = ref.data();
        const uint8_t* fp = frame.data();
        uint8_t*       dp = diff.data();
        for (int i = 0; i < N; ++i) {
            int d = (int)fp[i] - (int)rp[i];
            dp[i] = (uint8_t)(d < 0 ? -d : d);
        }

        // 2) blur (gaussian on diff to suppress per-pixel noise)
        xi::Image blurred = (blur_r > 0) ? xi::ops::gaussian(diff, blur_r) : diff;

        // 3) threshold
        xi::Image mask(W, H, 1);
        const uint8_t* bp = blurred.data();
        uint8_t*       mp = mask.data();
        for (int i = 0; i < N; ++i) {
            mp[i] = (bp[i] > thresh) ? 255 : 0;
        }

        // 4) morphological close (helps thin scratches survive)
        xi::Image cleaned = (close_r > 0) ? xi::ops::close(mask, close_r) : mask;

        // 5) connected regions
        auto regions = xi::ops::findFilledRegions(cleaned);

        // 6) area filter — find LARGEST in [min_a, max_a]
        int largest_area = 0;
        const std::vector<xi::ops::Point>* largest = nullptr;
        int kept = 0, total = (int)regions.size();
        for (auto& r : regions) {
            int a = (int)r.size();
            if (a < min_a || a > max_a) continue;
            ++kept;
            if (a > largest_area) { largest_area = a; largest = &r; }
        }

        bool defect_present = (kept > 0);
        int bx0 = -1, by0 = -1, bx1 = -1, by1 = -1;
        if (largest) {
            auto bb = xi::ops::bbox(*largest);
            bx0 = bb.x; by0 = bb.y;
            bx1 = bb.x + bb.w - 1;
            by1 = bb.y + bb.h - 1;
        }
        double score = (min_a > 0) ? (double)largest_area / (double)min_a : 0.0;

        last_present_ = defect_present;
        last_largest_ = largest_area;
        last_total_   = total;
        last_kept_    = kept;
        last_bx0_ = bx0; last_by0_ = by0; last_bx1_ = bx1; last_by1_ = by1;
        ++frames_processed_;

        return xi::Record()
            .image("diff",    diff)
            .image("blurred", blurred)
            .image("mask",    cleaned)
            .set("defect_present", defect_present)
            .set("score",          score)
            .set("largest_area",   largest_area)
            .set("kept",           kept)
            .set("total_regions",  total)
            .set("bbox_x0",        bx0)
            .set("bbox_y0",        by0)
            .set("bbox_x1",        bx1)
            .set("bbox_y1",        by1)
            .set("blur_radius",    blur_r)
            .set("diff_threshold", thresh)
            .set("close_radius",   close_r)
            .set("min_area",       min_a)
            .set("max_area",       max_a);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if      (command == "set_blur_radius")    blur_radius_    = clamp_int(p["value"].as_int(blur_radius_),    0, 20);
        else if (command == "set_diff_threshold") diff_threshold_ = clamp_int(p["value"].as_int(diff_threshold_), 1, 254);
        else if (command == "set_close_radius")   close_radius_   = clamp_int(p["value"].as_int(close_radius_),   0, 20);
        else if (command == "set_min_area")       min_area_       = clamp_int(p["value"].as_int(min_area_),       1, 1000000);
        else if (command == "set_max_area")       max_area_       = clamp_int(p["value"].as_int(max_area_),       1, 1000000);
        else if (command == "reset") {
            blur_radius_ = 2; diff_threshold_ = 18; close_radius_ = 2;
            min_area_ = 25; max_area_ = 5000;
        }
        if (max_area_ < min_area_) max_area_ = min_area_;
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("blur_radius",    blur_radius_)
            .set("diff_threshold", diff_threshold_)
            .set("close_radius",   close_radius_)
            .set("min_area",       min_area_)
            .set("max_area",       max_area_)
            .set("frames_processed", frames_processed_)
            .set("last_present",   last_present_)
            .set("last_largest",   last_largest_)
            .set("last_total",     last_total_)
            .set("last_kept",      last_kept_)
            .set("last_bbox_x0",   last_bx0_)
            .set("last_bbox_y0",   last_by0_)
            .set("last_bbox_x1",   last_bx1_)
            .set("last_bbox_y1",   last_by1_)
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        blur_radius_    = p["blur_radius"].as_int(blur_radius_);
        diff_threshold_ = p["diff_threshold"].as_int(diff_threshold_);
        close_radius_   = p["close_radius"].as_int(close_radius_);
        min_area_       = p["min_area"].as_int(min_area_);
        max_area_       = p["max_area"].as_int(max_area_);
        if (max_area_ < min_area_) max_area_ = min_area_;
        return true;
    }

private:
    int blur_radius_    = 2;
    int diff_threshold_ = 18;
    int close_radius_   = 2;
    int min_area_       = 25;
    int max_area_       = 5000;

    int    frames_processed_ = 0;
    bool   last_present_     = false;
    int    last_largest_     = 0;
    int    last_total_       = 0;
    int    last_kept_        = 0;
    int    last_bx0_ = -1, last_by0_ = -1, last_bx1_ = -1, last_by1_ = -1;

    static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
};

XI_PLUGIN_IMPL(GoldenDefectFinder)
