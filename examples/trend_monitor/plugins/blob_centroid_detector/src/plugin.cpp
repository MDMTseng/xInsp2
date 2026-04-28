//
// blob_centroid_detector.cpp — find dark circular blobs on a noisy
// gradient background and emit their centroids.
//
// One pass:
//
//   blurred  = gaussian(src, blur_radius)
//   bg       = boxBlur(blurred, block_radius)
//   mask     = (bg - blurred) > diff_C        // dark on bright
//   cleaned  = close(mask, close_radius)
//   regions  = findFilledRegions(cleaned)
//   filter regions by [min_area, max_area]
//   centroid_i = (mean(x), mean(y)) over the region's pixel set
//
// Why this is a single plugin (and not three like circle_counting):
//
// The script that uses this plugin tracks blob identities across
// frames. The downstream consumer is the *list of centroids*, not a
// labelled mask or a count. Splitting "make mask" / "label regions"
// / "extract centroids" into separate plugins would force the script
// to glue them every run and would not give the user any extra
// tunable in return: every slider here belongs to the centroid
// extraction pipeline as a whole.
//
// process(input):
//   input image "src"   : 1-channel grayscale frame
//   per-frame overrides: input["blur_radius" | "block_radius"
//                              | "diff_C" | "close_radius"
//                              | "min_area" | "max_area"]
//
//   returns:
//     image "mask"        : pre-morphology binary
//     image "cleaned"     : post-close binary
//     "count"             : int — accepted regions
//     "total_regions"     : int — pre-area-filter
//     "rejected_small"    : int
//     "rejected_big"      : int
//     "centroids"         : array of {"x":int,"y":int,"area":int}
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_ops.hpp>

#include <cstdint>
#include <vector>

class BlobCentroidDetector : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        const xi::Image& src = input.get_image("src");
        if (src.empty() || src.channels != 1) {
            return xi::Record().set("error",
                std::string("blob_centroid_detector: need 1-channel 'src' image"));
        }

        int blur_r  = input["blur_radius"].as_int(blur_radius_);
        int block_r = input["block_radius"].as_int(block_radius_);
        int diff_c  = input["diff_C"].as_int(diff_C_);
        int close_r = input["close_radius"].as_int(close_radius_);
        int min_a   = input["min_area"].as_int(min_area_);
        int max_a   = input["max_area"].as_int(max_area_);
        if (block_r < 1) block_r = 1;
        if (close_r < 0) close_r = 0;
        if (max_a   < min_a) max_a = min_a;

        // 1) Smooth + local-mean background.
        xi::Image blurred = (blur_r > 0) ? xi::ops::gaussian(src, blur_r) : src;
        xi::Image bg      = xi::ops::boxBlur(blurred, block_r);

        // 2) Threshold: pixel is "dark vs local bg" when bg - blurred > C.
        xi::Image mask(src.width, src.height, 1);
        const uint8_t* sp = blurred.data();
        const uint8_t* bp = bg.data();
        uint8_t*       mp = mask.data();
        const int N = src.width * src.height;
        for (int i = 0; i < N; ++i) {
            int diff = (int)bp[i] - (int)sp[i];
            mp[i] = (diff > diff_c) ? 255 : 0;
        }

        // 3) Morph-close to seal noise pinholes inside the disc.
        xi::Image cleaned = (close_r > 0) ? xi::ops::close(mask, close_r) : mask;

        // 4) Connected components → centroids.
        auto regions = xi::ops::findFilledRegions(cleaned);

        int n_small = 0, n_big = 0, n_count = 0;
        // Build a JSON array via xi::Json so it nests correctly
        // through Record::set_raw / cJSON.
        xi::Json arr = xi::Json::array();
        for (auto& r : regions) {
            int area = (int)r.size();
            if (area < min_a) { ++n_small; continue; }
            if (area > max_a) { ++n_big;   continue; }
            ++n_count;
            // Centroid = mean of integer pixel coords (good enough for
            // tracking purposes — sub-pixel accuracy not needed since
            // per-frame motion is 4..20 px).
            long sx = 0, sy = 0;
            for (auto& pt : r) { sx += pt.x; sy += pt.y; }
            int cx = (int)(sx / (long)r.size());
            int cy = (int)(sy / (long)r.size());
            xi::Json c = xi::Json::object();
            c.set("x", cx);
            c.set("y", cy);
            c.set("area", area);
            arr.push(c);
        }

        last_count_ = n_count;
        last_total_ = (int)regions.size();
        last_small_ = n_small;
        last_big_   = n_big;
        ++frames_processed_;

        xi::Record out;
        out.image("mask",    mask)
           .image("cleaned", cleaned)
           .set("count",          n_count)
           .set("total_regions",  (int)regions.size())
           .set("rejected_small", n_small)
           .set("rejected_big",   n_big)
           .set("blur_radius",    blur_r)
           .set("block_radius",   block_r)
           .set("diff_C",         diff_c)
           .set("close_radius",   close_r)
           .set("min_area",       min_a)
           .set("max_area",       max_a);
        // Attach the centroids array as a raw cJSON child so the
        // script can read it via record["centroids"][i]["x"] etc.
        // Duplicate so `arr` (xi::Json owning) can RAII-free its
        // own copy when the function returns.
        out.set_raw("centroids", cJSON_Duplicate(arr.raw(), 1));
        return out;
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if      (command == "set_blur_radius")  blur_radius_  = clamp_int(p["value"].as_int(blur_radius_),  0, 20);
        else if (command == "set_block_radius") block_radius_ = clamp_int(p["value"].as_int(block_radius_), 1, 200);
        else if (command == "set_diff_C")       diff_C_       = clamp_int(p["value"].as_int(diff_C_),       1, 200);
        else if (command == "set_close_radius") close_radius_ = clamp_int(p["value"].as_int(close_radius_), 0, 10);
        else if (command == "set_min_area")     min_area_     = clamp_int(p["value"].as_int(min_area_),     0, 100000);
        else if (command == "set_max_area")     max_area_     = clamp_int(p["value"].as_int(max_area_),     1, 1000000);
        else if (command == "reset") {
            blur_radius_ = 2; block_radius_ = 40; diff_C_ = 15;
            close_radius_ = 1; min_area_ = 200; max_area_ = 4000;
        }
        if (max_area_ < min_area_) max_area_ = min_area_;
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("blur_radius",      blur_radius_)
            .set("block_radius",     block_radius_)
            .set("diff_C",           diff_C_)
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
        blur_radius_  = p["blur_radius"].as_int(blur_radius_);
        block_radius_ = p["block_radius"].as_int(block_radius_);
        diff_C_       = p["diff_C"].as_int(diff_C_);
        close_radius_ = p["close_radius"].as_int(close_radius_);
        min_area_     = p["min_area"].as_int(min_area_);
        max_area_     = p["max_area"].as_int(max_area_);
        if (max_area_ < min_area_) max_area_ = min_area_;
        return true;
    }

private:
    int blur_radius_  = 2;
    int block_radius_ = 40;
    int diff_C_       = 15;
    int close_radius_ = 1;
    int min_area_     = 200;
    int max_area_     = 4000;

    int frames_processed_ = 0;
    int last_count_       = 0;
    int last_total_       = 0;
    int last_small_       = 0;
    int last_big_         = 0;

    static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
};

XI_PLUGIN_IMPL(BlobCentroidDetector)
