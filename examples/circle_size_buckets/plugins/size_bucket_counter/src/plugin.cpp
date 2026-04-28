//
// size_bucket_counter.cpp — count + classify regions in a binary mask.
//
// Combines morphological cleanup, connected-component labelling, and
// area-bucket classification into one plugin. Designed for the
// circle_size_buckets project where each frame contains 8-12 dark
// circles drawn from one of three radius bands (small/medium/large)
// and the user wants per-bucket counts, not a flat total.
//
// process(input):
//   input image "mask"            : 1-channel binary mask (0 or 255)
//   per-frame overrides via input["close_radius"|"min_area"|"small_max"|
//                                 "medium_max"|"max_area"]
//
// Outputs:
//   image "cleaned"     — mask after morphological close
//   "count_small"       — regions with min_area <= a < small_max
//   "count_medium"      — regions with small_max  <= a < medium_max
//   "count_large"       — regions with medium_max <= a <= max_area
//   "total_regions"     — components found before area filtering
//   "rejected_small"    — components dropped as area < min_area
//   "rejected_big"      — components dropped as area > max_area
//
// The two cutoffs (small_max, medium_max) sit in the GAPS between the
// per-bucket area ranges (small ≤ 201, medium ∈ [380,615], large ∈
// [1018,1521]). Picking thresholds in the gap means morphological
// bleed of a few pixels never tips a circle into the wrong bucket.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_ops.hpp>

class SizeBucketCounter : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        const xi::Image& mask = input.get_image("mask");
        if (mask.empty() || mask.channels != 1) {
            return xi::Record().set("error",
                std::string("size_bucket_counter: need 1-channel 'mask' image"));
        }

        int close_r   = input["close_radius"].as_int(close_radius_);
        int min_a     = input["min_area"].as_int(min_area_);
        int small_max = input["small_max"].as_int(small_max_);
        int med_max   = input["medium_max"].as_int(medium_max_);
        int max_a     = input["max_area"].as_int(max_area_);
        if (close_r < 0)        close_r = 0;
        if (small_max < min_a)  small_max = min_a;
        if (med_max < small_max) med_max = small_max;
        if (max_a < med_max)    max_a = med_max;

        xi::Image cleaned = (close_r > 0) ? xi::ops::close(mask, close_r) : mask;
        auto regions = xi::ops::findFilledRegions(cleaned);

        int n_small = 0, n_med = 0, n_large = 0;
        int rej_small = 0, rej_big = 0;
        for (auto& r : regions) {
            int area = (int)r.size();
            if (area < min_a)        { ++rej_small; continue; }
            if (area > max_a)        { ++rej_big;   continue; }
            if (area < small_max)    ++n_small;
            else if (area < med_max) ++n_med;
            else                     ++n_large;
        }

        last_small_  = n_small;
        last_med_    = n_med;
        last_large_  = n_large;
        last_total_  = (int)regions.size();
        last_rej_s_  = rej_small;
        last_rej_b_  = rej_big;
        ++frames_processed_;

        return xi::Record()
            .image("cleaned",       cleaned)
            .set("count_small",     n_small)
            .set("count_medium",    n_med)
            .set("count_large",     n_large)
            .set("total_regions",   (int)regions.size())
            .set("rejected_small",  rej_small)
            .set("rejected_big",    rej_big)
            .set("close_radius",    close_r)
            .set("min_area",        min_a)
            .set("small_max",       small_max)
            .set("medium_max",      med_max)
            .set("max_area",        max_a);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if      (command == "set_close_radius") close_radius_ = clamp_int(p["value"].as_int(close_radius_), 0, 12);
        else if (command == "set_min_area")     min_area_     = clamp_int(p["value"].as_int(min_area_),     0, 100000);
        else if (command == "set_small_max")    small_max_    = clamp_int(p["value"].as_int(small_max_),    1, 100000);
        else if (command == "set_medium_max")   medium_max_   = clamp_int(p["value"].as_int(medium_max_),   1, 100000);
        else if (command == "set_max_area")     max_area_     = clamp_int(p["value"].as_int(max_area_),     1, 1000000);
        else if (command == "reset") {
            close_radius_ = 1; min_area_ = 20;
            small_max_ = 290; medium_max_ = 820; max_area_ = 2200;
        }
        // Keep cutoffs ordered.
        if (small_max_  < min_area_)  small_max_  = min_area_;
        if (medium_max_ < small_max_) medium_max_ = small_max_;
        if (max_area_   < medium_max_) max_area_   = medium_max_;
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("close_radius",     close_radius_)
            .set("min_area",         min_area_)
            .set("small_max",        small_max_)
            .set("medium_max",       medium_max_)
            .set("max_area",         max_area_)
            .set("frames_processed", frames_processed_)
            .set("last_small",       last_small_)
            .set("last_medium",      last_med_)
            .set("last_large",       last_large_)
            .set("last_total",       last_total_)
            .set("last_rej_small",   last_rej_s_)
            .set("last_rej_big",     last_rej_b_)
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        close_radius_ = p["close_radius"].as_int(close_radius_);
        min_area_     = p["min_area"].as_int(min_area_);
        small_max_    = p["small_max"].as_int(small_max_);
        medium_max_   = p["medium_max"].as_int(medium_max_);
        max_area_     = p["max_area"].as_int(max_area_);
        return true;
    }

private:
    int close_radius_ = 1;
    int min_area_     = 20;
    int small_max_    = 290;
    int medium_max_   = 820;
    int max_area_     = 2200;

    int frames_processed_ = 0;
    int last_small_ = 0, last_med_ = 0, last_large_ = 0;
    int last_total_ = 0, last_rej_s_ = 0, last_rej_b_ = 0;

    static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
};

XI_PLUGIN_IMPL(SizeBucketCounter)
