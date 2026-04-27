//
// reference_image.cpp — holds a golden-reference image in memory.
//
// Setup-time plugin: the user picks a reference image once (via UI or
// instance.json), and this plugin caches the decoded grayscale pixels.
// On every inspection run, `process()` returns the cached image to the
// script — no per-frame I/O.
//
// Why a plugin (not a Param): xi::Param is scalar-only, and we need to
// hold ~76KB of pixels. Instances are the natural home for state with
// custom lifecycle (load, validate, hand out).
//
// instance.json config:
//   { "reference_path": "<abs path to png>" }
//
// process(input):
//   returns {
//     image "reference" : grayscale uint8 (or empty if not loaded)
//     "loaded"          : bool
//     "path"            : string  (the path we tried)
//     "width","height"  : int
//     "error"           : string  (only on failure)
//   }
//
// exchange() commands (used by ui/index.html):
//   "get_status"             — returns get_def() (default branch)
//   "set_path" { value }     — set reference_path AND reload immediately
//   "reload"                 — re-read the current path
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

// stb_image is header-only; this plugin and png_frame_source each
// compile their own copy in their own translation unit, which is fine.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_GIF
#define STBI_NO_TGA
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_JPEG
#include "../vendor/stb_image.h"

#include <string>

class ReferenceImage : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& /*input*/) override {
        if (!loaded_ || cached_.empty()) {
            return xi::Record()
                .set("loaded", false)
                .set("path",   reference_path_)
                .set("error",  std::string(last_error_.empty()
                               ? "reference not loaded" : last_error_));
        }
        return xi::Record()
            .image("reference", cached_)
            .set("loaded", true)
            .set("path",   reference_path_)
            .set("width",  cached_.width)
            .set("height", cached_.height);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "set_path") {
            reference_path_ = p["value"].as_string(reference_path_);
            try_load();
        } else if (command == "reload") {
            try_load();
        }
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("reference_path", reference_path_)
            .set("loaded",         loaded_)
            .set("width",          loaded_ ? cached_.width  : 0)
            .set("height",         loaded_ ? cached_.height : 0)
            .set("channels",       loaded_ ? cached_.channels : 0)
            .set("error",          last_error_)
            .set("load_count",     load_count_)
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        std::string new_path = p["reference_path"].as_string("");
        if (!new_path.empty() && new_path != reference_path_) {
            reference_path_ = new_path;
            try_load();
        } else if (!new_path.empty() && !loaded_) {
            // same path but never loaded — try once
            reference_path_ = new_path;
            try_load();
        } else {
            reference_path_ = new_path;
        }
        return true;
    }

private:
    std::string reference_path_;
    xi::Image   cached_;
    bool        loaded_ = false;
    std::string last_error_;
    int         load_count_ = 0;

    void try_load() {
        last_error_.clear();
        if (reference_path_.empty()) {
            loaded_ = false;
            cached_ = {};
            last_error_ = "empty path";
            return;
        }
        int w = 0, h = 0, ch = 0;
        // force grayscale (1 channel) — defect comparison is on intensity
        unsigned char* px = stbi_load(reference_path_.c_str(), &w, &h, &ch, 1);
        if (!px) {
            loaded_ = false;
            cached_ = {};
            last_error_ = stbi_failure_reason() ? stbi_failure_reason() : "load failed";
            return;
        }
        cached_ = xi::Image(w, h, 1, px);
        stbi_image_free(px);
        loaded_ = true;
        ++load_count_;
    }
};

XI_PLUGIN_IMPL(ReferenceImage)
