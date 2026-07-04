//
// {{NAME}} — "expert" template: stateful synthetic image source (Layer 2).
//
// The SAME xi::Plugin skeleton as easy/medium, with one more layer: a
// background worker thread that PUSHES frames into the pipeline. While
// running it paints a frame every `interval_ms` and emits it as a sealed
// xi.pack@1 pack; a script reads it back from the trigger:
//     auto t = xi::current_trigger();
//     if (auto f = t.pack()) { auto img = f.get_image("frame"); ... }
//
// Shows the blessed source patterns — nothing hand-rolled:
//   - xi::spawn_worker for the SEH-safe worker thread (a stray fault on a
//     raw std::thread with no translator brings down the whole backend)
//   - pool_image() for a zero-copy frame + new_pack()/emit() to hand it to
//     the host. emit() seals the pack, dispatches it, and drops our ref —
//     one call that owns the whole builder_seal / emit_pack / release
//     refcount dance, NOT the manual C-API juggling that leaks on the
//     first early return.
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

    // A source PUSHES via emit() from its worker, so it has no per-call data
    // plane: this tier neither overrides process(xi::PackIn&, xi::PackOut&)
    // nor publishes XI_PLUGIN_PACK_DOOR (the door macro is only for plugins
    // that override the pack door). A source that ALSO wants an in-band
    // control door — closed-loop actuation from a script, effective on the
    // next emitted frame — overrides the door on top and adds the macro; see
    // plugins/mock_camera for that pattern.

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

            // Hand the frame to the host as a sealed xi.pack@1 pack — the sole
            // data plane (v12). new_pack() starts a host-side builder;
            // adopt_image hands the pool slot to the pack by refcount (no
            // memcpy — the pack addrefs, `frame` keeps its own ref); emit()
            // seals + dispatches and mints a fresh trigger id (ts = host now)
            // — one call, no manual refcount balance.
            xi::PackOut f = new_pack();
            f.i64("seq", (int64_t)seq);
            f.adopt_image("frame", w, h, 1, frame.pool_handle());
            emit(std::move(f));
            ++seq;
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

// No XI_PLUGIN_PACK_DOOR here on purpose: a pure source pushes via emit()
// and never overrides the pack door (see the note above process()'s slot).
XI_PLUGIN_IMPL({{CLASS}})
