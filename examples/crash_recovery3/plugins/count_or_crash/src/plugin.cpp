//
// count_or_crash.cpp — threshold + connectedComponents, crash on overflow.
//
// Validates xInsp2's plugin crash isolation. process() computes a blob count
// via cv::connectedComponents; if count > crash_when_count_above, it
// deliberately writes through a null pointer. That's a real SEH access
// violation — not a C++ throw — so it exercises the worker's
// _set_se_translator path, not the plugin's exception handling.
//
// Config:
//   { "crash_when_count_above": int }   default 8
//
// process(input):
//   input image "src"  : 1-channel grayscale (script converts RGB→GRAY)
//   returns:
//     image "mask"     : binary mask after fixed-threshold (128)
//     "count"          : int — connected component count (excluding background)
//
// exchange({command:"set_threshold", value:N}) updates crash_when_count_above
// live and returns get_def() per the plugin-abi convention.
//

#include <xi/xi_json.hpp>

class CountOrCrash : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        const xi::Image& src = input.get_image("src");
        if (src.empty() || src.channels != 1) {
            return xi::Record().set("error",
                std::string("count_or_crash: need 1-channel 'src' image"));
        }

        xi::Image mask = pool_image(src.width, src.height, 1);
        cv::threshold(src.as_cv_mat(), mask.as_cv_mat(),
                      128.0, 255.0, cv::THRESH_BINARY);

        cv::Mat labels;
        int n_labels = cv::connectedComponents(mask.as_cv_mat(), labels, 8, CV_32S);
        int count = std::max(0, n_labels - 1);  // skip background

        ++frames_processed_;
        last_count_ = count;

        if (count > crash_when_count_above_) {
            // Deliberate null deref. Volatile so the optimiser can't elide it.
            // SEH access violation lands here — not a C++ throw.
            *(volatile int*)0 = 42;
        }

        return xi::Record()
            .image("mask", mask)
            .set("count", count);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "set_threshold") {
            int v = p["value"].as_int(crash_when_count_above_);
            if (v < 0) v = 0;
            if (v > 1000000) v = 1000000;
            crash_when_count_above_ = v;
        }
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("crash_when_count_above", crash_when_count_above_)
            .set("frames_processed",       frames_processed_)
            .set("last_count",             last_count_)
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        crash_when_count_above_ = p["crash_when_count_above"].as_int(crash_when_count_above_);
        return true;
    }

private:
    int crash_when_count_above_ = 8;
    int frames_processed_       = 0;
    int last_count_             = 0;
};

XI_PLUGIN_IMPL(CountOrCrash)
