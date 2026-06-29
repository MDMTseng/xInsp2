// preview_sink.cpp — "view a script's output via a plugin", post-VAR.
//
// VAR + the core vars/preview wire were removed (branch refactor/remove-var-core).
// A script now surfaces what it wants to view by pushing a Record into a sink
// plugin instead of VAR'ing it:
//
//   xi::use("preview").process(
//       xi::Record().set("score", s).image("synth", img));
//
// This sink keeps the LATEST record's data JSON + image count; a UI (or this
// demo's test) pulls it back with exchange_instance({"command":"get_latest"}).
// It proves the new output model end to end using only surviving mechanisms
// (use().process + exchange_instance) — no core vars frame needed.
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <mutex>
#include <string>
#include <vector>

class PreviewSink : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // Capture the latest surfaced record. Images are only valid for THIS call;
    // we just record how many there were (a real preview plugin would JPEG-encode
    // + ship them through its own transport — out of scope for this validation).
    xi::Record process(const xi::Record& in) override {
        std::string data = in.data_json();
        int img_n = 0;
        for (auto& [k, img] : in.images()) { (void)k; if (!img.empty()) ++img_n; }

        std::lock_guard<std::mutex> lk(mu_);
        last_data_  = std::move(data);
        last_imgs_  = img_n;
        ++seen_;
        return xi::Record().set("received", (int64_t)seen_);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto c = p["command"].as_string();
        if (c == "clear") {
            std::lock_guard<std::mutex> lk(mu_);
            last_data_.clear(); last_imgs_ = 0;
        }
        return get_def();  // get_latest / get_def both return the current state
    }

    std::string get_def() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return xi::Json::object()
            .set("seen",        (int64_t)seen_)
            .set("data",        last_data_.empty() ? std::string("{}") : last_data_)
            .set("image_count", last_imgs_)
            .dump();
    }
    bool set_def(const std::string&) override { return true; }

private:
    mutable std::mutex mu_;
    std::string        last_data_ = "{}";
    int                last_imgs_ = 0;
    long long          seen_      = 0;
};

XI_PLUGIN_IMPL(PreviewSink)
