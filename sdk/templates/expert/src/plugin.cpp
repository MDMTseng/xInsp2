//
// {{NAME}} — "expert" template: stateful synthetic image source.
//
// Owns its own worker thread. While running, it pushes a frame into the
// trigger bus every `interval_ms` and counts each emit so a script can
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
#include <cJSON.h>            // backend ships cJSON in vendor/
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

class {{CLASS}} {
public:
    {{CLASS}}(const xi_host_api* host, const char* name)
        : host_(host), name_(name ? name : "") {}

    ~{{CLASS}}() {
        stop_();
    }

    const xi_host_api* host() const { return host_; }

    std::string get_def() const {
        std::lock_guard<std::mutex> lk(mu_);
        return std::string("{\"interval_ms\":") + std::to_string(interval_ms_)
             + ",\"width\":" + std::to_string(width_)
             + ",\"height\":" + std::to_string(height_)
             + ",\"running\":" + (running_.load() ? "true" : "false") + "}";
    }

    bool set_def(const std::string& json) {
        std::lock_guard<std::mutex> lk(mu_);
        // Pull simple numeric fields out by hand. (Real plugins should
        // use cJSON; this template avoids it to stay dependency-free.)
        auto pull_int = [&](const char* key, int& dst, int lo, int hi) {
            std::string needle = std::string("\"") + key + "\":";
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
    // The UI talks to us with JSON commands of the form
    //   { command: "start" | "stop" | "set_interval" | "set_size", value?: ... }
    // We accept the older bare-string forms ("start", "stop", "count")
    // for ad-hoc CLI calls too.
    std::string exchange(const std::string& cmd) {
        // JSON command path (UI panel)
        cJSON* root = cJSON_Parse(cmd.c_str());
        std::string command;
        if (root) {
            cJSON* c = cJSON_GetObjectItem(root, "command");
            if (c && cJSON_IsString(c)) command = c->valuestring;
            if (command == "set_interval") {
                cJSON* v = cJSON_GetObjectItem(root, "value");
                if (v && cJSON_IsNumber(v)) {
                    std::lock_guard<std::mutex> lk(mu_);
                    int n = (int)v->valuedouble;
                    interval_ms_ = n < 1 ? 1 : (n > 10000 ? 10000 : n);
                }
            } else if (command == "set_size") {
                cJSON* w = cJSON_GetObjectItem(root, "width");
                cJSON* h = cJSON_GetObjectItem(root, "height");
                std::lock_guard<std::mutex> lk(mu_);
                if (w && cJSON_IsNumber(w)) width_  = (int)w->valuedouble;
                if (h && cJSON_IsNumber(h)) height_ = (int)h->valuedouble;
            }
            cJSON_Delete(root);
        }
        if (!command.empty()) {
            if (command == "start") start_();
            if (command == "stop")  stop_();
        } else {
            // Bare-string fallback for human-typed exchange.
            if (cmd.find("start") != std::string::npos) start_();
            if (cmd.find("stop")  != std::string::npos) stop_();
        }
        // Report current status — the UI uses these to render counters.
        std::lock_guard<std::mutex> lk(mu_);
        return std::string("{\"running\":") + (running_.load() ? "true" : "false")
             + ",\"count\":" + std::to_string(emit_count_.load())
             + ",\"interval_ms\":" + std::to_string(interval_ms_)
             + ",\"width\":" + std::to_string(width_)
             + ",\"height\":" + std::to_string(height_) + "}";
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

XI_PLUGIN_IMPL({{CLASS}})
