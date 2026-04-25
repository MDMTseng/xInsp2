// projectPluginTemplates.ts — starter source for "Create Project Plugin"
// command. Three templates from minimal to advanced; each one is heavily
// commented so the file itself doubles as a tutorial that walks the
// developer through one chunk of the plugin API at a time.
//
// What gets shared across templates:
//   - <className>(host, name) constructor
//   - host()                       → const xi_host_api*
//   - get_def() / set_def(json)    → JSON config schema
//   - process(Record) → Record     → main per-trigger processing
//   - exchange(cmd) → string       → RPC channel for script-side calls
//   - XI_PLUGIN_IMPL(<className>)  → bottom of file, exports the C ABI
//
// xi_plugin_support.hpp is force-included by the project-plugin compile
// path (CompileMode::PluginDev) so #includes for xi_abi/xi_image/xi_ops/
// xi_record are already in scope — the templates intentionally don't
// re-include them.

export type TemplateId = 'easy' | 'medium' | 'expert';

export interface TemplateChoice {
    id: TemplateId;
    label: string;
    description: string;
    detail: string;
}

export const TEMPLATE_CHOICES: TemplateChoice[] = [
    {
        id: 'easy',
        label: '$(symbol-class)  Easy — pass-through',
        description: 'Minimal plugin shell',
        detail: 'A do-nothing class wired up correctly. Best place to start: shows constructor / def / process / exchange in 30 lines. Edit process() and you have a working plugin.',
    },
    {
        id: 'medium',
        label: '$(symbol-method)  Medium — image processor',
        description: 'Reads input image, applies threshold, emits binary image + blob count',
        detail: 'Demonstrates the Record image API, JSON-driven config (threshold from set_def), and how to publish a result image back to the script.',
    },
    {
        id: 'expert',
        label: '$(symbol-event)  Expert — stateful source',
        description: 'Background worker thread that emits trigger frames into the bus',
        detail: 'A simulated camera. Shows xi::spawn_worker for safe threading, host_api->emit_trigger for pushing into the bus, persistent state across DLL reloads, and exchange() as a control channel (start/stop).',
    },
];

// Build the .cpp body for the chosen template. `name` is the plugin
// folder name = display name, sanitized into a C++ class identifier.
export function renderPluginCpp(template: TemplateId, name: string): string {
    const className = toClassName(name);
    if (template === 'easy')   return easyTemplate(className, name);
    if (template === 'medium') return mediumTemplate(className, name);
    return expertTemplate(className, name);
}

export function renderPluginJson(name: string, description: string): string {
    return JSON.stringify({
        name,
        description,
        dll: `${name}.dll`,
        factory: 'xi_plugin_create',
        has_ui: false,
    }, null, 2) + '\n';
}

function toClassName(folder: string): string {
    // PascalCase, drop non-alnum. "my_filter" → "MyFilter", "blob 2" → "Blob2".
    const parts = folder.split(/[^A-Za-z0-9]+/).filter(Boolean);
    if (parts.length === 0) return 'MyPlugin';
    return parts.map(p => p[0].toUpperCase() + p.slice(1)).join('');
}

// ---------------------------------------------------------------------------
// Easy — a minimal pass-through plugin with line-by-line tutorial comments.
// ---------------------------------------------------------------------------
function easyTemplate(cls: string, folder: string): string {
    return `//
// ${folder} — minimal "easy" project plugin.
//
// This file is auto-generated from a template. Read top-to-bottom — the
// comments walk through every member function in the plugin API.
//
// You don't need to #include anything: xi_plugin_support.hpp is force-
// included by the project-plugin compile path, which pulls in xi_abi.hpp
// (the XI_PLUGIN_IMPL macro), xi_image.hpp (xi::Image), xi_ops.hpp (the
// portable image ops), and xi_record.hpp (xi::Record).
//

#include <string>

class ${cls} {
public:
    // ---- 1. Constructor ----------------------------------------------------
    //
    // Called once when the script (or backend) instantiates this plugin.
    //   host : opaque handle into the BACKEND's image pool. Pass-through
    //          to xi::record_from_c / record_to_c — never read directly.
    //   name : the instance name from project.json (e.g. "filter0").
    //          Useful for log lines and naming sub-resources.
    //
    ${cls}(const xi_host_api* host, const char* name)
        : host_(host), name_(name ? name : "") {}

    // host() accessor is REQUIRED by XI_PLUGIN_IMPL — it's how the macro
    // forwards process() through the host's pool.
    const xi_host_api* host() const { return host_; }

    // ---- 2. get_def / set_def — JSON config -------------------------------
    //
    // get_def() returns a JSON object describing this instance's
    // configuration. The UI panel reads this to render a form, and the
    // project save serializes it. Keep it stable — instance.json on disk
    // round-trips through these two functions.
    //
    std::string get_def() const {
        return "{}";
    }

    bool set_def(const std::string& /*json*/) {
        // Parse 'json' (a JSON object string) and apply to self.
        // Return true on success, false if the JSON was rejected.
        return true;
    }

    // ---- 3. process — the main hook ---------------------------------------
    //
    // Called every time the inspection script does:
    //     auto out = ${folder}.process(in_record);
    //
    // 'in' carries images keyed by name and a JSON blob; the returned
    // Record is what the script sees. Empty Record is a valid no-op.
    //
    xi::Record process(const xi::Record& /*in*/) {
        return xi::Record{};
    }

    // ---- 4. exchange — RPC channel ----------------------------------------
    //
    // Lets the script call:
    //     auto rsp = xi::use<${cls}>("${folder}").exchange("hello");
    // We return a JSON string. Use this for "do X right now" actions that
    // don't fit the per-frame process() flow.
    //
    std::string exchange(const std::string& /*cmd*/) {
        return "{}";
    }

private:
    const xi_host_api* host_;
    std::string        name_;
};

// XI_PLUGIN_IMPL emits the C ABI thunks (xi_plugin_create, _destroy,
// _process, _exchange, _get_def, _set_def) that the backend's loader
// resolves at LoadLibrary time. ALWAYS at file scope, after the class.
XI_PLUGIN_IMPL(${cls})
`;
}

// ---------------------------------------------------------------------------
// Medium — an image-processing plugin: input image → threshold → output.
// Shows the Record image API and JSON-driven config.
// ---------------------------------------------------------------------------
function mediumTemplate(cls: string, folder: string): string {
    return `//
// ${folder} — "medium" template: image processor.
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

class ${cls} {
public:
    ${cls}(const xi_host_api* host, const char* name)
        : host_(host), name_(name ? name : "") {}

    const xi_host_api* host() const { return host_; }

    // ---- Config: a single 'threshold' int, 0..255, default 128 ------------
    std::string get_def() const {
        // The shape returned here is what the UI renders + what
        // project.json stores. Keys must match what set_def parses.
        return std::string("{\\"threshold\\":") + std::to_string(threshold_) + "}";
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
        return out;
    }

    std::string exchange(const std::string& /*cmd*/) {
        return "{}";
    }

private:
    const xi_host_api* host_;
    std::string        name_;
    int                threshold_ = 128;
};

XI_PLUGIN_IMPL(${cls})
`;
}

// ---------------------------------------------------------------------------
// Expert — a stateful image source. Owns a worker thread; emits frames
// into the trigger bus on a fixed cadence. Driven via exchange().
// ---------------------------------------------------------------------------
function expertTemplate(cls: string, folder: string): string {
    return `//
// ${folder} — "expert" template: stateful synthetic image source.
//
// Owns its own worker thread. While running, it pushes a frame into the
// trigger bus every \`interval_ms\` and counts each emit so a script can
// read frame count via exchange("count").
//
// Shows:
//   - Background thread management with xi::spawn_worker (SEH-safe)
//   - host_api emit_trigger to push images into the bus zero-copy
//   - Persistent config across reloads (set_def from project.json)
//   - exchange() as a start/stop/query channel
//
// Open with care: this is the most-touchy template. Read top-to-bottom.
//

#include <xi/xi_thread.hpp>   // xi::spawn_worker
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

class ${cls} {
public:
    ${cls}(const xi_host_api* host, const char* name)
        : host_(host), name_(name ? name : "") {}

    ~${cls}() {
        stop_();
    }

    const xi_host_api* host() const { return host_; }

    std::string get_def() const {
        std::lock_guard<std::mutex> lk(mu_);
        return std::string("{\\"interval_ms\\":") + std::to_string(interval_ms_)
             + ",\\"width\\":" + std::to_string(width_)
             + ",\\"height\\":" + std::to_string(height_)
             + ",\\"running\\":" + (running_.load() ? "true" : "false") + "}";
    }

    bool set_def(const std::string& json) {
        std::lock_guard<std::mutex> lk(mu_);
        // Pull simple numeric fields out by hand. (Real plugins should
        // use cJSON; this template avoids it to stay dependency-free.)
        auto pull_int = [&](const char* key, int& dst, int lo, int hi) {
            std::string needle = std::string("\\"") + key + "\\":";
            auto p = json.find(needle);
            if (p == std::string::npos) return;
            try {
                int v = std::stoi(json.substr(p + needle.size()));
                if (v < lo) v = lo;
                if (v > hi) v = hi;
                dst = v;
            } catch (...) {}
        };
        pull_int("interval_ms", interval_ms_, 1,    10000);
        pull_int("width",       width_,       1,    8192);
        pull_int("height",      height_,      1,    8192);
        return true;
    }

    // Sources don't usually emit a per-call result — they push via the
    // trigger bus instead. We still expose a no-op process() because
    // XI_PLUGIN_IMPL requires it.
    xi::Record process(const xi::Record& /*in*/) { return xi::Record{}; }

    // Control channel. cmd is freeform; we recognise:
    //   "start"  → spin up the worker
    //   "stop"   → join the worker
    //   "count"  → return {"count": N}
    std::string exchange(const std::string& cmd) {
        if (cmd.find("start") != std::string::npos) { start_(); return "{\\"ok\\":true}"; }
        if (cmd.find("stop")  != std::string::npos) { stop_();  return "{\\"ok\\":true}"; }
        if (cmd.find("count") != std::string::npos) {
            return std::string("{\\"count\\":") + std::to_string(emit_count_.load()) + "}";
        }
        return "{}";
    }

private:
    void start_() {
        std::lock_guard<std::mutex> lk(mu_);
        if (running_.load()) return;
        running_ = true;
        worker_ = xi::spawn_worker(name_ + "-source", [this] { loop_(); });
    }

    void stop_() {
        running_ = false;
        if (worker_.joinable()) worker_.join();
    }

    void loop_() {
        // Build a tiny grayscale image once and reuse the buffer.
        // For a real camera you'd grab from the SDK here.
        std::vector<uint8_t> buf((size_t)width_ * height_, 0);
        uint64_t seq = 0;
        while (running_.load()) {
            // Animate the buffer so frames are visibly different.
            uint8_t v = (uint8_t)(seq & 0xff);
            std::memset(buf.data(), v, buf.size());
            ++seq;

            // Allocate a backend pool image and copy into it. The host
            // API is the only legal way to allocate images that the
            // backend (and other plugins) can see.
            xi_image_handle h = host_->image_create(width_, height_, 1);
            if (h != XI_IMAGE_NULL) {
                std::memcpy(host_->image_data(h), buf.data(), buf.size());

                // Build a one-image record and push it onto the bus
                // under our own source name.
                xi_record_image rec_img{ "frame", h };
                xi_trigger_id   tid{ seq, (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() };
                int64_t         ts_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch()).count();
                if (host_->emit_trigger) {
                    host_->emit_trigger(name_.c_str(), tid, ts_us, &rec_img, 1);
                }
                emit_count_.fetch_add(1);
                // emit_trigger addrefs internally; release our ref.
                host_->image_release(h);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
        }
    }

    const xi_host_api* host_;
    std::string        name_;
    mutable std::mutex mu_;
    int                interval_ms_ = 100;
    int                width_       = 640;
    int                height_      = 480;
    std::atomic<bool>  running_{false};
    std::atomic<uint64_t> emit_count_{0};
    std::thread        worker_;
};

XI_PLUGIN_IMPL(${cls})
`;
}
