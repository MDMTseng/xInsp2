//
// count_or_crash.cpp — deliberate crash plugin for validating xInsp2
// process-isolation recovery.
//
// process(input):
//   input image "src"  : 1-channel grayscale
//
//   1. Threshold src at 127.
//   2. cv::connectedComponents on the binary mask.
//   3. If count > crash_when_count_above → write to *(volatile int*)0
//      to trigger a real access-violation (SEH). NOT a C++ throw —
//      we want to test the worker's SEH wrapper, not exception
//      propagation across the ABI.
//   4. Otherwise return {count, mask}.
//
// exchange:
//   {"command":"set_threshold","value":int}  → get_def() after mutation
//
// get_def / set_def: persist `crash_when_count_above`.
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
        cv::threshold(src.as_cv_mat(), mask.as_cv_mat(), 127.0, 255.0, cv::THRESH_BINARY);

        // connectedComponents labels every 8-connected component of non-zero
        // pixels (label 0 = background). The plugin's notion of "count" is
        // (n_labels - 1) so the empty mask reports 0.
        cv::Mat labels;
        int n_labels = cv::connectedComponents(mask.as_cv_mat(), labels, 8, CV_32S);
        int count = (n_labels > 0) ? (n_labels - 1) : 0;

        last_count_ = count;
        ++frames_processed_;

        if (count > crash_when_count_above_) {
            // Real SEH crash. `volatile` keeps the optimiser from eliding it.
            *(volatile int*)nullptr = 42;
            // Unreachable, but quiet the warning:
            return xi::Record();
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
            crash_when_count_above_ = v;
        } else if (command == "reset") {
            crash_when_count_above_ = 8;
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
