//
// {{NAME}} — "medium" template: image processor (Layer 1).
//
// The SAME xi::Plugin skeleton as the easy template, with two layers turned
// on: configurable params (get_def/set_def + an exchange RPC channel, all via
// xi::Json) and a live status() line. It reads an input image keyed "gray",
// thresholds it, and writes back:
//   image  "binary"    — the thresholded result (uint8, 1 channel)
//   number "fg_pct"     — fraction of foreground pixels, 0.0 .. 1.0
//
// Demonstrates the blessed patterns: pool_image() for a zero-copy output,
// the xi::as_cv_read / xi::as_cv_write bridges, xi::Json for parsing, and
// status() for the operator channel.
//
// xi_plugin_support.hpp is force-included (xi::Plugin, xi::Image,
// pool_image, the cv bridges). xi::Json is the one extra header a config
// plugin pulls in.
//

#include <xi/xi_json.hpp>

#include <atomic>
#include <string>

class {{CLASS}} : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // ---- Config: a single 'threshold' int, 0..255 -------------------------
    //
    // get_def() is what the UI renders + what project.json stores; set_def()
    // is its inverse. instance.json round-trips through this pair, so the keys
    // must match. Built and parsed with xi::Json — no hand-rolled string
    // scanning (the anti-pattern the SDK README warns against).
    //
    std::string get_def() const override {
        return xi::Json::object()
            .set("threshold", threshold_.load())
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        threshold_ = clamp_(p["threshold"].as_int(threshold_.load()));
        return true;
    }

    // ---- process: image in → image out ------------------------------------
    xi::Record process(const xi::Record& in) override {
        const xi::Image& src = in.get_image("gray");
        if (src.empty()) return {};

        // A fresh single-channel output allocated IN THE HOST POOL — cv:: writes
        // land in pool memory, so returning it is zero-copy (no heap→pool
        // memcpy across the ABI). This is the standard way to produce an output.
        xi::Image dst = pool_image(src.width, src.height, 1);

        // Read the INPUT through as_cv_read (const view — never mutate a shared
        // input) and WRITE the OUTPUT through as_cv_write. Collapse a colour
        // input to gray first so the threshold has a single channel to work on.
        cv::Mat srcMat = xi::as_cv_read(src);
        cv::Mat gray;
        if (srcMat.channels() == 1) gray = srcMat;
        else                        cv::cvtColor(srcMat, gray, cv::COLOR_BGR2GRAY);

        cv::Mat out = xi::as_cv_write(dst);
        cv::threshold(gray, out, (double)threshold_.load(), 255.0, cv::THRESH_BINARY);

        const double pixels = (double)src.width * src.height;
        const double fg_pct = pixels > 0 ? (double)cv::countNonZero(out) / pixels : 0.0;
        last_fg_pct_ = fg_pct;
        status("thr=" + std::to_string(threshold_.load()) +
               " fg=" + std::to_string(fg_pct));

        // .image(key, img) builds the image map; .set(key, val) chains
        // numbers / strings / bools into the JSON payload.
        return xi::Record()
            .image("binary", dst)
            .set("fg_pct",    fg_pct)
            .set("threshold", (double)threshold_.load());
    }

    // ---- exchange: the UI / script RPC channel ----------------------------
    //
    // The UI panel posts { command: "set_threshold", value: N } (and a
    // "get_status" poll); the host wraps it into JSON and lands it here. We
    // parse with xi::Json, apply, and return the current status the UI renders.
    //
    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "set_threshold")
            threshold_ = clamp_(p["value"].as_int(threshold_.load()));
        else if (command != "get_status" && !command.empty())
            return exchange_unknown_command(command);
        return xi::Json::object()
            .set("threshold",   threshold_.load())
            .set("last_fg_pct", last_fg_pct_.load())
            .dump();
    }

private:
    static int clamp_(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

    // process() runs on a dispatch worker while exchange()/set_def() run on the
    // host's control thread — atomics keep the shared config race-free without a
    // lock. (A plugin with richer coupled state guards it with a mutex instead;
    // see plugins/expose, or config_swap_probe for the frame-perfect swap.)
    std::atomic<int>    threshold_{128};
    std::atomic<double> last_fg_pct_{0.0};
};

XI_PLUGIN_IMPL({{CLASS}})
