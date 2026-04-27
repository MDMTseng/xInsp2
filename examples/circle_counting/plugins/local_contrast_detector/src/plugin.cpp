//
// local_contrast_detector.cpp — gradient-tolerant binary mask plugin.
//
// The classic recipe for "find dark spots on a non-uniform bright
// background":
//
//     blurred  = gaussian(src, blur_radius)        // suppress noise
//     bg       = boxBlur(blurred, block_radius)    // local mean
//     mask     = polarity * (bg - blurred) > diff_C
//
// `polarity = +1` finds DARK regions on a brighter background (default —
// matches the circle-counting test set). `polarity = -1` finds BRIGHT
// regions on a darker background.
//
// Generalising this op-pair into a plugin (vs inlining the lambda the
// previous agent shipped) lets the user retune blur / block / threshold
// from the sidebar without recompiling, and lets the same plugin be
// reused for any "spot vs gradient" task.
//
// Usage from script:
//   auto out = xi::use("det").process(xi::Record().image("src", gray));
//   auto mask = out.get_image("mask");
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_ops.hpp>

#include <cstdint>

class LocalContrastDetector : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        const xi::Image& src = input.get_image("src");
        if (src.empty() || src.channels != 1) {
            return xi::Record().set("error",
                std::string("local_contrast_detector: need 1-channel 'src' image"));
        }

        // Per-frame override: input fields override stored config.
        int  blur_r  = input["blur_radius"].as_int(blur_radius_);
        int  block_r = input["block_radius"].as_int(block_radius_);
        int  diff_c  = input["diff_C"].as_int(diff_C_);
        int  pol     = input["polarity"].as_int(polarity_);
        if (pol != -1 && pol != 1) pol = 1;

        xi::Image blurred = (blur_r > 0) ? xi::ops::gaussian(src, blur_r) : src;
        xi::Image bg      = xi::ops::boxBlur(blurred, block_r);

        xi::Image mask(src.width, src.height, 1);
        const uint8_t* sp = blurred.data();
        const uint8_t* bp = bg.data();
        uint8_t*       mp = mask.data();
        const int n = src.width * src.height;
        long sum = 0;
        for (int i = 0; i < n; ++i) {
            int diff = (int)bp[i] - (int)sp[i];   // positive when src darker than bg
            int signed_diff = pol * diff;
            uint8_t v = (signed_diff > diff_c) ? 255 : 0;
            mp[i] = v;
            sum += v;
        }
        last_mean_  = (n > 0) ? (double)sum / (double)n : 0.0;
        ++frames_processed_;

        return xi::Record()
            .image("mask", mask)
            .image("blurred", blurred)
            .image("bg", bg)
            .set("mask_mean",       last_mean_)
            .set("blur_radius",     blur_r)
            .set("block_radius",    block_r)
            .set("diff_C",          diff_c)
            .set("polarity",        pol);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if      (command == "set_blur_radius")  blur_radius_  = clamp_int(p["value"].as_int(blur_radius_),  0, 20);
        else if (command == "set_block_radius") block_radius_ = clamp_int(p["value"].as_int(block_radius_), 1, 200);
        else if (command == "set_diff_C")       diff_C_       = clamp_int(p["value"].as_int(diff_C_),       1, 200);
        else if (command == "set_polarity") {
            int v = p["value"].as_int(polarity_);
            polarity_ = (v < 0) ? -1 : 1;
        } else if (command == "reset") {
            blur_radius_ = 2; block_radius_ = 40; diff_C_ = 15; polarity_ = 1;
        }
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("blur_radius",      blur_radius_)
            .set("block_radius",     block_radius_)
            .set("diff_C",           diff_C_)
            .set("polarity",         polarity_)
            .set("frames_processed", frames_processed_)
            .set("mask_mean",        last_mean_)
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        blur_radius_  = p["blur_radius"].as_int(blur_radius_);
        block_radius_ = p["block_radius"].as_int(block_radius_);
        diff_C_       = p["diff_C"].as_int(diff_C_);
        polarity_     = p["polarity"].as_int(polarity_);
        if (polarity_ != -1) polarity_ = 1;
        return true;
    }

private:
    int    blur_radius_  = 2;
    int    block_radius_ = 40;
    int    diff_C_       = 15;
    int    polarity_     = 1;     // +1 = dark on bright
    int    frames_processed_ = 0;
    double last_mean_    = 0.0;

    static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
};

XI_PLUGIN_IMPL(LocalContrastDetector)
