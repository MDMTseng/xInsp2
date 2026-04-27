//
// {{NAME}} — "medium" template: image processor.
//
// Reads an input image keyed "gray" from the input Record, applies a
// threshold, and writes back two outputs:
//   image "binary"   — the thresholded result (uint8, 1 channel)
//   number "fg_pct"  — fraction of foreground pixels, 0.0 .. 1.0
//
// Demonstrates: xi::Record image access, set_def parsing, output Record
// building, and xi::Image element-wise work.
//

#include <cJSON.h>     // backend ships cJSON in vendor/, also force-include path
#include <cstdint>
#include <string>

class {{CLASS}} {
public:
    {{CLASS}}(const xi_host_api* host, const char* name)
        : host_(host), name_(name ? name : "") {}

    const xi_host_api* host() const { return host_; }

    // ---- Config: a single 'threshold' int, 0..255, default 128 ------------
    std::string get_def() const {
        // The shape returned here is what the UI renders + what
        // project.json stores. Keys must match what set_def parses.
        return std::string("{\"threshold\":") + std::to_string(threshold_) + "}";
    }

    bool set_def(const std::string& json) {
        // Tiny inline parse — the backend is happy to use cJSON, but
        // for one int we keep it dependency-free so the template
        // compiles even if the user later trims includes.
        cJSON* root = cJSON_Parse(json.c_str());
        if (!root) return false;
        cJSON* t = cJSON_GetObjectItem(root, "threshold");
        if (t && cJSON_IsNumber(t)) {
            int v = (int)t->valuedouble;
            if (v < 0)   v = 0;
            if (v > 255) v = 255;
            threshold_ = v;
        }
        cJSON_Delete(root);
        return true;
    }

    // ---- process: image in → image out ------------------------------------
    xi::Record process(const xi::Record& in) {
        xi::Record out;

        // Pull the input image. images() returns a name → xi::Image map.
        const auto& imgs = in.images();
        auto it = imgs.find("gray");
        if (it == imgs.end() || it->second.empty()) return out;
        const xi::Image& src = it->second;

        // Make an output image the same size, single-channel.
        xi::Image bin(src.width, src.height, 1);

        const uint8_t* sp = src.data();
        uint8_t*       dp = bin.data();
        const int total = src.width * src.height * src.channels;
        long long fg = 0;
        // For multi-channel input, threshold against the first channel
        // (most cameras give us pre-grayscaled 1-ch data anyway).
        for (int i = 0, j = 0; i < total; i += src.channels, ++j) {
            uint8_t hot = (sp[i] >= (uint8_t)threshold_) ? 255 : 0;
            dp[j] = hot;
            if (hot) ++fg;
        }
        const long long pixels = (long long)src.width * src.height;
        const double fg_pct = pixels > 0 ? (double)fg / (double)pixels : 0.0;

        // Record API: .image(key, img) builds the image map; .set(key, val)
        // chains numbers / strings / bools into the JSON payload.
        out.image("binary", bin);
        out.set("fg_pct", fg_pct);
        out.set("threshold", (double)threshold_);
        last_fg_pct_ = fg_pct;
        return out;
    }

    // The UI panel talks to us through here. We accept either a "raw"
    // get-status query or a JSON command the UI posts via
    // webview.postMessage({ type: 'exchange', cmd: { command: ..., value: ... } }).
    // The extension wraps that into JSON and lands here.
    std::string exchange(const std::string& cmd) {
        cJSON* root = cJSON_Parse(cmd.c_str());
        if (root) {
            cJSON* c = cJSON_GetObjectItem(root, "command");
            if (c && cJSON_IsString(c) && std::string(c->valuestring) == "set_threshold") {
                cJSON* v = cJSON_GetObjectItem(root, "value");
                if (v && cJSON_IsNumber(v)) {
                    int n = (int)v->valuedouble;
                    if (n < 0)   n = 0;
                    if (n > 255) n = 255;
                    threshold_ = n;
                }
            }
            cJSON_Delete(root);
        }
        // Return current status in a UI-friendly shape.
        return std::string("{\"threshold\":") + std::to_string(threshold_)
             + ",\"last_fg_pct\":" + std::to_string(last_fg_pct_) + "}";
    }

private:
    const xi_host_api* host_;
    std::string        name_;
    int                threshold_ = 128;
    double             last_fg_pct_ = 0.0;
};

XI_PLUGIN_IMPL({{CLASS}})
