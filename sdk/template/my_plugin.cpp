//
// my_plugin.cpp — your starting point.
//
// This file is a working image-thresholder that demonstrates every
// pattern in README.md. Each numbered section is a self-contained
// example you can keep, adapt, or strip out.
//
//   PATTERN 1: image processor (image in → image out)
//   PATTERN 2: command routing  (UI clicks → exchange handler)
//   PATTERN 3: state persistence (get_def/set_def)
//   PATTERN 4: per-frame override (input["x"] beats stored config)
//   PATTERN 5: per-instance file storage (folder_path() for big data)
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

class MyPlugin : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // ---- PATTERN 1: image processor ----------------------------------
    //
    // process() runs per frame. Read input.get_image(...) for inputs;
    // build outputs into a fresh xi::Image and put it in the result Record.
    // input.get_image returns an empty Image if the key is missing —
    // always check .empty() before using.
    //
    // ---- PATTERN 4: per-frame override -------------------------------
    //
    // Allow the script to override your stored config for one call by
    // passing the field in the input Record. as_int(default) returns
    // the stored value if input doesn't carry it.
    //
    xi::Record process(const xi::Record& input) override {
        const xi::Image& src = input.get_image("src");
        if (src.empty()) {
            // No image — nothing to do, but don't crash. Reporting an
            // error in the result Record makes the script's job easier.
            return xi::Record().set("error", "no 'src' image in input");
        }

        // Per-frame override: input["threshold"] wins; fall back to stored.
        int t = input["threshold"].as_int(threshold_);
        bool inv = input["invert"].as_bool(invert_);

        xi::Image dst(src.width, src.height, src.channels);
        const uint8_t* sp = src.data();
        uint8_t*       dp = dst.data();
        const int n = src.width * src.height * src.channels;
        for (int i = 0; i < n; ++i) {
            bool above = sp[i] > (uint8_t)t;
            dp[i] = (inv ? !above : above) ? 255 : 0;
        }

        // Cache the last result so save_threshold_image (PATTERN 5) can
        // write it without recomputing.
        last_result_ = dst;
        ++frames_processed_;

        return xi::Record()
            .image("dst", dst)
            .set("threshold_used", t)
            .set("invert_used", inv)
            .set("frames_processed", frames_processed_);
    }

    // ---- PATTERN 2: command routing + UI round-trip ------------------
    //
    // The full webview ↔ plugin loop, end to end:
    //
    //   [user clicks button in webview]
    //         ↓ vscode.postMessage({type:'exchange', cmd:{command:'set_x', value:42}})
    //   [extension's webview wrapper receives it]
    //         ↓ sendCmd('exchange_instance', {name, cmd}) over WebSocket
    //   [backend invokes THIS exchange(json_string)]   ← we are here
    //         ↓ we parse, mutate state, return get_def()
    //   [backend sends rsp.data = our returned string]
    //         ↓ extension parses, posts to webview
    //   [webview's message listener gets {type:'status', ...parsed_reply}]
    //         ↓ UI script updates DOM
    //
    // Key points:
    //   - We don't "push" to the UI. We RETURN a JSON string. The
    //     extension wraps it in {type:'status', ...} for the webview.
    //   - That's why returning get_def() is the canonical pattern: it
    //     lets the UI re-render whatever changed without us caring
    //     which fields the UI actually displays.
    //   - Parse with xi::Json (RAII, no manual cJSON_Delete).
    //   - Unknown commands fall through to get_def() — that's also how
    //     the UI's initial 'get_status' poll works.
    //
    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();

        if (command == "set_threshold") {
            int v = p["value"].as_int(threshold_);
            if (v < 0)   v = 0;
            if (v > 255) v = 255;
            threshold_ = v;
        } else if (command == "set_invert") {
            invert_ = p["value"].as_bool(invert_);
        } else if (command == "reset") {
            threshold_ = 128; invert_ = false; frames_processed_ = 0;
        } else if (command == "save_threshold_image") {
            // PATTERN 5 — see below.
            last_save_path_ = save_last_result_to_folder();
        }
        // 'get_status' (and unknown commands) just fall through to the
        // current state via get_def().
        return get_def();
    }

    // ---- PATTERN 3: state persistence --------------------------------
    //
    // get_def() must serialize EVERYTHING your set_def() needs to
    // reconstruct the plugin. The host stores it in instance.json on
    // project save and re-applies it on load. Keep it small (a few
    // hundred bytes); for big data use the folder pattern below.
    //
    std::string get_def() const override {
        return xi::Json::object()
            .set("threshold",         threshold_)
            .set("invert",            invert_)
            .set("frames_processed",  frames_processed_)
            .set("last_save_path",    last_save_path_)
            .set("folder",            folder_path())
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        threshold_        = p["threshold"].as_int(threshold_);
        invert_           = p["invert"].as_bool(invert_);
        frames_processed_ = p["frames_processed"].as_int(0);
        last_save_path_   = p["last_save_path"].as_string("");
        return true;
    }

private:
    int         threshold_        = 128;
    bool        invert_           = false;
    int         frames_processed_ = 0;
    xi::Image   last_result_;
    std::string last_save_path_;

    // ---- PATTERN 5: per-instance file storage ------------------------
    //
    // folder_path() returns <project>/instances/<this-instance-name>/.
    // Already created by the host before the plugin was constructed;
    // never deleted by the host; survives hot-reload + project
    // reopen. Perfect for calibration files, reference images, ML
    // weights — anything bigger than the JSON config.
    //
    std::string save_last_result_to_folder() {
        if (last_result_.empty()) return "";
        std::string folder = folder_path();
        if (folder.empty()) return ""; // running detached from a project
        std::filesystem::create_directories(folder);
        auto path = std::filesystem::path(folder) / "last_threshold.raw";

        std::ofstream f(path.string(), std::ios::binary);
        const int n = last_result_.width * last_result_.height * last_result_.channels;
        f.write((const char*)last_result_.data(), n);
        return path.string();
    }
};

XI_PLUGIN_IMPL(MyPlugin)
