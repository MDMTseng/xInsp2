//
// histogram.cpp — image analysis with a rich, nested output.
//
// Demonstrates:
//   - reading an input image entry from the pack
//   - computing statistics
//   - building an output pack with scalar stats (mean, stddev, peak bin)
//     PLUS a nested array (counts[256]) as ONE canonical-msgpack entry —
//     nesting is msgpack's job in the pack plane (xi::mp::Writer; a reader
//     decodes it with xi::mp::Reader)
//   - a live UI that polls the latest histogram via exchange("get_status")
//

#include <xi/xi_abi.hpp>
#include <xi/xi_mp.hpp>     // canonical msgpack writer for the nested counts array

#include <cmath>
#include <mutex>
#include <string>

class Histogram : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // The xi.pack@1 door: image "src" in → stats + nested counts out.
    void process(xi::PackIn& in, xi::PackOut& out) override {
        auto img = in.image("src");
        if (!img || !img->pixels) {
            out.fault("missing_input", "src", "histogram: no 'src' image");
            return;
        }

        // Build 256-bin histogram. For multi-channel input, average channels.
        int counts[256] = {0};
        const uint8_t* p = static_cast<const uint8_t*>(img->pixels);
        const int pixels = img->width * img->height;
        const int ch = img->channels;
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
        const double mean   = pixels > 0 ? sum / pixels : 0;
        const double var    = pixels > 0 ? (sum2 / pixels) - (mean * mean) : 0;
        const double stddev = var > 0 ? std::sqrt(var) : 0;

        // Scalars ride as typed entries; the counts[256] array rides as one
        // nested canonical-msgpack entry (tag XI_PACK_TAG_MP). A consumer
        // decodes it with xi::mp::Reader — see plugins/blob_analysis for the
        // read side of this pattern.
        out.i64("pixels",     pixels);
        out.f64("mean",       mean);
        out.f64("stddev",     stddev);
        out.i64("peak_bin",   peak_bin);
        out.i64("peak_count", peak_count);

        xi::mp::Writer mw;
        mw.array(256);
        for (int i = 0; i < 256; ++i) mw.int_(counts[i]);
        out.mp("counts", mw.bytes().data(), mw.bytes().size());

        // Keep a JSON snapshot for the UI's exchange poll (below). process()
        // runs on a dispatch worker; exchange() on the control thread.
        std::string snap = "{\"pixels\":" + std::to_string(pixels) +
            ",\"mean\":"       + std::to_string(mean) +
            ",\"stddev\":"     + std::to_string(stddev) +
            ",\"peak_bin\":"   + std::to_string(peak_bin) +
            ",\"peak_count\":" + std::to_string(peak_count) +
            ",\"counts\":[";
        for (int i = 0; i < 256; ++i) {
            if (i) snap += ",";
            snap += std::to_string(counts[i]);
        }
        snap += "]}";
        std::lock_guard<std::mutex> lk(mu_);
        last_json_ = std::move(snap);
    }

    // The UI polls {command:"get_status"} and renders the latest histogram.
    std::string exchange(const std::string& /*cmd*/) override {
        std::lock_guard<std::mutex> lk(mu_);
        return last_json_;
    }

private:
    std::mutex  mu_;
    std::string last_json_ = "{}";
};

XI_PLUGIN_IMPL(Histogram)
// Publish the xi.pack@1 door (the sole data plane since the v12 ABI cut).
XI_PLUGIN_PACK_DOOR(Histogram)
