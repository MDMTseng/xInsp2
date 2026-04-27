//
// region_counter.cpp — count blobs in a binary mask, with morphology + area filter.
//
// process(input):
//   input image "mask"   : 1-channel binary mask (0 or 255)
//   per-frame overrides: input["close_radius"|"min_area"|"max_area"]
//
//   returns:
//     image "cleaned"    : mask after morphological close
//     "count"            : int — accepted regions
//     "total_regions"    : int — pre-area-filter count
//     "rejected_small"   : int
//     "rejected_big"     : int
//
// Replaces the inline morphology + findFilledRegions block from the
// previous circle-counting inspect.cpp. Generalising it gives the user
// three sliders (close radius, min area, max area) and a numeric
// per-bucket readout so they can tell whether the filter is too tight
// or too loose without stepping outside the IDE.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_ops.hpp>

class RegionCounter : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        const xi::Image& mask = input.get_image("mask");
        if (mask.empty() || mask.channels != 1) {
            return xi::Record().set("error",
                std::string("region_counter: need 1-channel 'mask' image"));
        }

        int close_r  = input["close_radius"].as_int(close_radius_);
        int min_a    = input["min_area"].as_int(min_area_);
        int max_a    = input["max_area"].as_int(max_area_);
        if (close_r < 0) close_r = 0;
        if (max_a < min_a) max_a = min_a;

        xi::Image cleaned = (close_r > 0) ? xi::ops::close(mask, close_r) : mask;
        auto regions = xi::ops::findFilledRegions(cleaned);

        int n_count = 0, n_small = 0, n_big = 0;
        for (auto& r : regions) {
            int area = (int)r.size();
            if (area < min_a) { ++n_small; continue; }
            if (area > max_a) { ++n_big;   continue; }
            ++n_count;
        }

        last_count_   = n_count;
        last_total_   = (int)regions.size();
        last_small_   = n_small;
        last_big_     = n_big;
        ++frames_processed_;

        return xi::Record()
            .image("cleaned", cleaned)
            .set("count",          n_count)
            .set("total_regions",  (int)regions.size())
            .set("rejected_small", n_small)
            .set("rejected_big",   n_big)
            .set("close_radius",   close_r)
            .set("min_area",       min_a)
            .set("max_area",       max_a);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if      (command == "set_close_radius") close_radius_ = clamp_int(p["value"].as_int(close_radius_), 0, 20);
        else if (command == "set_min_area")     min_area_     = clamp_int(p["value"].as_int(min_area_),     0, 100000);
        else if (command == "set_max_area")     max_area_     = clamp_int(p["value"].as_int(max_area_),     1, 1000000);
        else if (command == "reset") { close_radius_ = 2; min_area_ = 300; max_area_ = 4000; }
        if (max_area_ < min_area_) max_area_ = min_area_;
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("close_radius",     close_radius_)
            .set("min_area",         min_area_)
            .set("max_area",         max_area_)
            .set("frames_processed", frames_processed_)
            .set("last_count",       last_count_)
            .set("last_total",       last_total_)
            .set("last_small",       last_small_)
            .set("last_big",         last_big_)
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        close_radius_ = p["close_radius"].as_int(close_radius_);
        min_area_     = p["min_area"].as_int(min_area_);
        max_area_     = p["max_area"].as_int(max_area_);
        return true;
    }

private:
    int close_radius_ = 2;
    int min_area_     = 300;
    int max_area_     = 4000;
    int frames_processed_ = 0;
    int last_count_ = 0, last_total_ = 0, last_small_ = 0, last_big_ = 0;

    static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
};

XI_PLUGIN_IMPL(RegionCounter)
