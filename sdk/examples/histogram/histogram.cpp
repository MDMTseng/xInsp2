//
// histogram.cpp — image analysis with a rich JSON output.
//
// Demonstrates:
//   - reading an input image
//   - computing statistics
//   - building a Record that contains a nested array (counts[256])
//     + scalar stats (mean, stddev, peak bin)
//   - a live UI that visualizes the histogram
//

#include <xi/xi_abi.hpp>

#include <cmath>
#include <cstring>

class Histogram : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        const xi::Image& img = input.get_image("src");
        if (img.empty()) return xi::Record().set("error", "no 'src' image");

        // Build 256-bin histogram. For multi-channel input, average channels.
        int counts[256] = {0};
        const uint8_t* p = img.data();
        const int pixels = img.width * img.height;
        const int ch = img.channels;
        if (ch == 1) {
            for (int i = 0; i < pixels; ++i) counts[p[i]]++;
        } else {
            for (int i = 0; i < pixels; ++i) {
                int sum = 0;
                for (int c = 0; c < ch; ++c) sum += p[i * ch + c];
                counts[sum / ch]++;
            }
        }

        // Stats
        double sum = 0, sum2 = 0;
        int peak_bin = 0, peak_count = 0;
        for (int i = 0; i < 256; ++i) {
            sum  += (double)counts[i] * i;
            sum2 += (double)counts[i] * i * i;
            if (counts[i] > peak_count) { peak_count = counts[i]; peak_bin = i; }
        }
        const double mean   = sum / pixels;
        const double var    = (sum2 / pixels) - (mean * mean);
        const double stddev = var > 0 ? std::sqrt(var) : 0;

        // Build the output record. Use cJSON directly for the array field.
        xi::Record out;
        out.set("pixels", pixels);
        out.set("mean",   mean);
        out.set("stddev", stddev);
        out.set("peak_bin", peak_bin);
        out.set("peak_count", peak_count);

        cJSON* arr = cJSON_CreateArray();
        for (int i = 0; i < 256; ++i) cJSON_AddItemToArray(arr, cJSON_CreateNumber(counts[i]));
        out.set_raw("counts", arr);

        return out;
    }
};

XI_PLUGIN_IMPL(Histogram)
