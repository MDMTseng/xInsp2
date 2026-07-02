//
// {{NAME}} — "expert" template: stateful synthetic image source (Layer 2).
//
// The SAME xi::Plugin skeleton as easy/medium, with one more layer: a
// background worker thread that PUSHES frames into the pipeline. While
// running it paints a frame every `interval_ms` and emits it; a script reads
// it back as xi::current_trigger().image("frame").
//
// Shows the blessed source patterns — nothing hand-rolled:
//   - xi::spawn_worker for the SEH-safe worker thread (a stray fault on a
//     raw std::thread with no translator brings down the whole backend)
//   - pool_image() for a zero-copy frame + emit() to hand it to the host.
//     emit() fills host()/name() and runs the RAII marshal/refcount path —
//     NOT the manual host_->image_create / emit_record / image_release
//     juggling that leaks on the first early return.
//   - xi::Json for config + the exchange control channel
//   - status() for the operator line
//
// Open with care: this is the most-touchy template. Read top-to-bottom.
//

#include <xi/xi_json.hpp>     // xi::Json config + command parsing
#include <xi/xi_thread.hpp>   // xi::spawn_worker (SEH-safe)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

class {{CLASS}} : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    ~{{CLASS}}() override { stop_(); }

    std::string get_def() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return xi::Json::object()
            .set("interval_ms", interval_ms_)
            .set("width",       width_)
            .set("height",      height_)
            .set("running",     running_.load())
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        std::lock_guard<std::mutex> lk(mu_);
        interval_ms_ = clamp_(p["interval_ms"].as_int(interval_ms_), 1, 10000);
        width_       = clamp_(p["width"].as_int(width_),             1, 8192);
        height_      = clamp_(p["height"].as_int(height_),           1, 8192);
        return true;
    }

    // A source pushes via emit() from its worker, so per-call process() is a
    // no-op — but the base class already gives us that default, so this tier
    // doesn't even override it.

    // Control channel. The UI posts { command: "start" | "stop" |
    // "set_interval"(value) | "set_size"(width,height) | "get_status" }.
    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "start") start_();
        else if (command == "stop") stop_();
        else if (command == "set_interval") {
            std::lock_guard<std::mutex> lk(mu_);
            interval_ms_ = clamp_(p["value"].as_int(interval_ms_), 1, 10000);
        } else if (command == "set_size") {
            std::lock_guard<std::mutex> lk(mu_);
            width_  = clamp_(p["width"].as_int(width_),   1, 8192);
            height_ = clamp_(p["height"].as_int(height_), 1, 8192);
        } else if (command != "get_status" && !command.empty()) {
            return exchange_unknown_command(command);
        }
        // Report current status — the UI uses these to render counters.
        std::lock_guard<std::mutex> lk(mu_);
        return xi::Json::object()
            .set("running",     running_.load())
            .set("count",       (int)emit_count_.load())
            .set("interval_ms", interval_ms_)
            .set("width",       width_)
            .set("height",      height_)
            .dump();
    }

private:
    static int clamp_(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    void start_() {
        if (running_.exchange(true)) return;
        status("running");
        worker_ = xi::spawn_worker(name() + "-source", [this] { loop_(); });
    }

    void stop_() {
        if (!running_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
        status("stopped");
    }

    void loop_() {
        uint64_t seq = 0;
        while (running_.load()) {
            int w, h, iv;
            { std::lock_guard<std::mutex> lk(mu_); w = width_; h = height_; iv = interval_ms_; }

            // Zero-copy: paint straight into a fresh host pool slot. For a real
            // camera you'd copy the grabbed frame in here.
            xi::Image frame = pool_image(w, h, 1);
            std::memset(frame.write(), (uint8_t)(seq & 0xFF), (size_t)w * h);
            ++seq;

            // Hand the frame to the host. emit() fills host()/name(), marshals
            // the image through the pool refcount path, and mints a fresh
            // trigger id (ts = host now) — one call, no manual refcount balance.
            xi::Record rec;
            rec.image("frame", frame);
            emit(rec);
            emit_count_.fetch_add(1);

            std::this_thread::sleep_for(std::chrono::milliseconds(iv));
        }
    }

    mutable std::mutex    mu_;
    int                   interval_ms_ = 100;
    int                   width_       = 640;
    int                   height_      = 480;
    std::atomic<bool>     running_{false};
    std::atomic<uint64_t> emit_count_{0};
    std::thread           worker_;
};

XI_PLUGIN_IMPL({{CLASS}})
