// circle_counter — tuned via cmd:recompile_project_plugin loop.
//
// Erosion radius reduced from 8 (which destroyed r=11 circles) to 5,
// which preserves all singles after the contrast threshold.
//
// Dataset wrinkle: generate.py places circles with no overlap check,
// so some frames have two touching circles that survive erosion as one
// figure-8 region. Counting raw regions therefore under-counts those
// frames. Fix: for each region compute its bounding-box aspect ratio
// (longest/shortest side). A single circle has aspect ~1; a pair of
// touching circles has aspect ~2. We round aspect to the nearest int
// (>=1) and credit that many singles per region. Circle-radius
// variation (11..17) keeps singles well under aspect 1.5.

#include <xi/xi.hpp>
#include <xi/xi_ops.hpp>

#include <algorithm>
#include <climits>
#include <vector>

using namespace xi::ops;

class CircleCounter : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        auto src = input.get_image("src");
        if (src.empty()) return xi::Record().set("count", 0);

        // Local-contrast mask.
        auto blurred = gaussian(src, 2);
        auto bg      = boxBlur(blurred, 40);

        xi::Image mask(src.width, src.height, 1);
        const uint8_t* sp = blurred.data();
        const uint8_t* bp = bg.data();
        uint8_t* dp = mask.data();
        int n = src.width * src.height;
        for (int i = 0; i < n; ++i) {
            int diff = (int)bp[i] - (int)sp[i];
            dp[i] = diff > 18 ? 255 : 0;
        }

        // Light erosion to clean speckle without devouring r=11 blobs.
        auto cleaned = erode(mask, 5);

        auto regions = findFilledRegions(cleaned);

        // For each region in the area band, count singles via bbox aspect.
        int count = 0;
        for (auto& r : regions) {
            int area = (int)r.size();
            if (area < 50 || area > 5000) continue;
            int xmin = INT_MAX, xmax = INT_MIN, ymin = INT_MAX, ymax = INT_MIN;
            for (auto& p : r) {
                if (p.x < xmin) xmin = p.x;
                if (p.x > xmax) xmax = p.x;
                if (p.y < ymin) ymin = p.y;
                if (p.y > ymax) ymax = p.y;
            }
            int w = xmax - xmin + 1;
            int h = ymax - ymin + 1;
            int lo = std::min(w, h);
            int hi = std::max(w, h);
            int n_in_region = (hi + lo / 2) / lo;  // round(hi/lo)
            if (n_in_region < 1) n_in_region = 1;
            count += n_in_region;
        }

        return xi::Record()
            .image("mask", cleaned)
            .set("count", count);
    }
};

XI_PLUGIN_IMPL(CircleCounter)
