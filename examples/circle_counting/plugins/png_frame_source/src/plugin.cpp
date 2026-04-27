//
// png_frame_source.cpp — load PNG frames from a configurable folder.
//
// Replaces the hardcoded-FRAMES_RAW_DIR + frame_idx hack the previous
// circle-counting agent shipped. The directory is an instance-def
// field (xi::Param is scalar-only, so a path can't be a Param), so the
// user can repoint this at any folder of frames without recompiling.
//
// process(input):
//   input["idx"]        : int   — frame index (default 0)
//   returns {
//     image "frame"      : the loaded grayscale image
//     "loaded"           : bool — true on success
//     "path"             : string — what was attempted
//     "width","height"   : int   — dims of loaded frame
//   }
//
// exchange() commands (used by ui/index.html):
//   "get_status"               — fall through, returns get_def()
//   "set_frames_dir" {value}   — set frames directory
//   "set_filename_pattern"     — printf-style pattern for the filename
//   "set_pad_width"            — zero-pad width for %d substitution
//   "set_force_grayscale"      — bool
//   "probe" {value: idx}       — try loading idx and report result
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

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

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

class PngFrameSource : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        int idx = input["idx"].as_int(0);
        std::string path = build_path(idx);
        last_path_ = path;
        last_idx_  = idx;

        int w = 0, h = 0, ch = 0;
        // Force 1 channel (grayscale) when force_grayscale_, else native.
        int desired = force_grayscale_ ? 1 : 0;
        unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &ch, desired);
        if (!pixels) {
            last_loaded_ = false;
            last_w_ = last_h_ = 0;
            return xi::Record()
                .set("loaded", false)
                .set("path",   path)
                .set("error",  std::string(stbi_failure_reason()
                                           ? stbi_failure_reason() : "unknown"));
        }
        int actual_ch = (desired ? desired : ch);
        xi::Image img(w, h, actual_ch, pixels);
        stbi_image_free(pixels);

        last_loaded_ = true;
        last_w_      = w;
        last_h_      = h;
        ++loaded_count_;

        return xi::Record()
            .image("frame", img)
            .set("loaded", true)
            .set("path",   path)
            .set("width",  w)
            .set("height", h)
            .set("channels", actual_ch);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "set_frames_dir") {
            frames_dir_ = p["value"].as_string(frames_dir_);
        } else if (command == "set_filename_pattern") {
            filename_pattern_ = p["value"].as_string(filename_pattern_);
        } else if (command == "set_pad_width") {
            pad_width_ = p["value"].as_int(pad_width_);
            if (pad_width_ < 0) pad_width_ = 0;
            if (pad_width_ > 8) pad_width_ = 8;
        } else if (command == "set_force_grayscale") {
            force_grayscale_ = p["value"].as_bool(force_grayscale_);
        } else if (command == "probe") {
            int idx = p["value"].as_int(0);
            std::string path = build_path(idx);
            int w = 0, h = 0, ch = 0;
            int ok = stbi_info(path.c_str(), &w, &h, &ch);
            return xi::Json::object()
                .set("probe_path",   path)
                .set("probe_ok",     (bool)ok)
                .set("probe_width",  w)
                .set("probe_height", h)
                .set("probe_channels", ch)
                .set("frames_dir",   frames_dir_)
                .set("filename_pattern", filename_pattern_)
                .set("pad_width",    pad_width_)
                .set("force_grayscale", force_grayscale_)
                .set("last_path",    last_path_)
                .set("last_loaded",  last_loaded_)
                .set("last_idx",     last_idx_)
                .set("loaded_count", loaded_count_)
                .dump();
        }
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("frames_dir",       frames_dir_)
            .set("filename_pattern", filename_pattern_)
            .set("pad_width",        pad_width_)
            .set("force_grayscale",  force_grayscale_)
            .set("last_path",        last_path_)
            .set("last_loaded",      last_loaded_)
            .set("last_idx",         last_idx_)
            .set("last_width",       last_w_)
            .set("last_height",      last_h_)
            .set("loaded_count",     loaded_count_)
            .set("folder",           folder_path())
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        frames_dir_       = p["frames_dir"].as_string(frames_dir_);
        filename_pattern_ = p["filename_pattern"].as_string(filename_pattern_);
        pad_width_        = p["pad_width"].as_int(pad_width_);
        force_grayscale_  = p["force_grayscale"].as_bool(force_grayscale_);
        return true;
    }

private:
    std::string frames_dir_       = "";
    // Pattern uses {idx} as the placeholder so we don't have to play games
    // with printf format strings coming from JSON. Pad width is separate.
    std::string filename_pattern_ = "frame_{idx}.png";
    int         pad_width_        = 2;
    bool        force_grayscale_  = true;

    std::string last_path_;
    bool        last_loaded_ = false;
    int         last_idx_    = 0;
    int         last_w_      = 0;
    int         last_h_      = 0;
    int         loaded_count_ = 0;

    std::string build_path(int idx) const {
        // Substitute {idx} with the zero-padded index.
        char num[16];
        if (pad_width_ > 0)
            std::snprintf(num, sizeof(num), "%0*d", pad_width_, idx);
        else
            std::snprintf(num, sizeof(num), "%d", idx);
        std::string fname = filename_pattern_;
        auto pos = fname.find("{idx}");
        if (pos != std::string::npos) fname.replace(pos, 5, num);
        if (frames_dir_.empty()) return fname;
        return (std::filesystem::path(frames_dir_) / fname).string();
    }
};

XI_PLUGIN_IMPL(PngFrameSource)
