//
// count_or_crash.cpp — counts connected components in a thresholded image.
// If the count exceeds `crash_when_count_above`, deliberately null-derefs
// to test xInsp2's plugin crash isolation. The crash is a real SEH access
// violation, not a C++ throw — we want to exercise the framework, not
// the standard exception machinery.
//
// process(input):
//   input image "src"  : grayscale (1ch). RGB falls back to RGB->GRAY.
//   per-frame override : input["crash_when_count_above"]
//
//   on the happy path:
//     image "mask"   : 1-channel binary mask after threshold@127
//     "count"        : int (connectedComponents - 1)
//
//   on the unhappy path:
//     never returns; segfaults via null write before reaching the return.
//

#include <xi/xi_json.hpp>

class CountOrCrash : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        const xi::Image& src = input.get_image("src");
        if (src.empty()) {
            return xi::Record().set("error",
                std::string("count_or_crash: missing 'src' image"));
        }

        // Ensure 1-channel input. RGB inputs get a defensive cvtColor so
        // the test harness can't accidentally pass colour and skew the
        // count via channel artefacts.
        cv::Mat gray;
        if (src.channels == 1) {
            gray = src.as_cv_mat();
        } else {
            cv::cvtColor(src.as_cv_mat(), gray, cv::COLOR_RGB2GRAY);
        }

        // Threshold (dark blobs on bright bg) -> binary mask.
        xi::Image mask = pool_image(src.width, src.height, 1);
        cv::threshold(gray, mask.as_cv_mat(), 127, 255, cv::THRESH_BINARY_INV);

        // Count connected components, excluding background label 0.
        cv::Mat labels;
        int n_labels = cv::connectedComponents(mask.as_cv_mat(), labels, 8, CV_32S);
        int count = std::max(0, n_labels - 1);

        ++frames_processed_;
        last_count_ = count;

        int limit = input["crash_when_count_above"].as_int(crash_when_count_above_);
        if (count > limit) {
            // Hard crash. Volatile so the optimiser can't elide the store;
            // null-deref on Windows raises an SEH access violation that
            // bypasses every C++ try/catch (and the _set_se_translator
            // shim, since we expect this plugin to run in the worker
            // process under default isolation).
            *(volatile int*)nullptr = 42;
            // unreachable
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
    int frames_processed_ = 0;
    int last_count_       = 0;
};

XI_PLUGIN_IMPL(CountOrCrash)
