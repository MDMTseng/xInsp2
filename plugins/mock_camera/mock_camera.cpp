//
// mock_camera.cpp — simulated camera plugin for xInsp2.
//
// Generates RGB test images with a frame counter rendered in the top-left.
// Configurable: width, height, fps. Supports start/stop streaming.
//
// Build (from plugins/mock_camera/):
//   cl /std:c++20 /LD /EHsc /MD /O2 /utf-8 /I../../backend/include
//      mock_camera.cpp /Fe:mock_camera.dll
//

#include <xi/xi_abi.hpp>       // xi::Plugin, xi::PackOut, xi::Image, pool_image()/emit()
#include <xi/xi_thread.hpp>    // xi::spawn_worker — SEH-safe capture thread
#include <xi/xi_json.hpp>      // parses set_def/exchange (canonical over cmd.find)
#include <xi/xi_contract.hpp>  // fail-loud command inputs + schema-skew errors

#include "mock_camera_keys.gen.h"

#include <atomic>
#include <chrono>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

// Digit bitmaps for rendering the frame counter (5x7 font)
static const uint8_t DIGITS[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
    {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
};

static void draw_digit(xi::Image& img, int dx, int dy, int digit) {
    if (digit < 0 || digit > 9) return;
    for (int row = 0; row < 7; ++row) {
        uint8_t bits = DIGITS[digit][row];
        for (int col = 0; col < 5; ++col) {
            if (bits & (1 << (4 - col))) {
                int x = dx + col * 2;
                int y = dy + row * 2;
                for (int py = 0; py < 2; ++py) {
                    for (int px = 0; px < 2; ++px) {
                        int fx = x + px, fy = y + py;
                        if (fx >= 0 && fx < img.width && fy >= 0 && fy < img.height) {
                            int i = (fy * img.width + fx) * img.channels;
                            img.data()[i + 0] = 255;
                            img.data()[i + 1] = 255;
                            img.data()[i + 2] = 255;
                        }
                    }
                }
            }
        }
    }
}

static void draw_number(xi::Image& img, int x, int y, int number) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", number);
    int dx = x;
    for (int i = 0; buf[i]; ++i) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            draw_digit(img, dx, y, buf[i] - '0');
            dx += 12; // 5*2 + 2 spacing
        }
    }
}

namespace keys = xi::mock_camera::keys;

class MockCamera : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    MockCamera(const xi_host_api* host, const char* name) : xi::Plugin(host, name) {}

    ~MockCamera() override { stop_(); }

    std::string get_def() const override {
        return xi::Json::object()
            .set(keys::kWidth, w_.load())
            .set(keys::kHeight, h_.load())
            .set(keys::kFps, fps_.load())
            .set(keys::kStreaming, running_.load())
            .set(keys::kPackMode, pack_mode_.load())
            .set(keys::kGain, gain_.load())
            .dump();
    }

    bool set_def(const std::string& j) override {
        auto p = xi::Json::parse(j);
        if (!p.valid()) return false;

        // Guard 3: reject a config built against an incompatible header version
        // with a precise error naming both versions. Absent stamp (a legacy
        // persisted instance.json) is tolerated.
        auto sv = p[xi::contract::kSchemaKey];
        if (sv.is_number() && sv.as_int() != xi::mock_camera::kSchemaVersion) {
            log_error("mock_camera: config schema mismatch: built for v" +
                      std::to_string(sv.as_int()) + ", this plugin serves v" +
                      std::to_string(xi::mock_camera::kSchemaVersion));
            return false;
        }

        if (p[keys::kWidth].as_int(0)  > 0) w_ = p[keys::kWidth].as_int();
        if (p[keys::kHeight].as_int(0) > 0) h_ = p[keys::kHeight].as_int();
        if (p[keys::kFps].as_int(0)    > 0) fps_ = clamp_fps_(p[keys::kFps].as_int());
        // polaris2 wave-2: opt into pack-plane emit (default false). Absent key
        // leaves the current mode (a legacy config keeps emitting Records).
        if (p[keys::kPackMode].valid()) pack_mode_ = p[keys::kPackMode].as_bool(false);
        // polaris2 ex-feedback: pack-mode brightness multiplier (default 1.0).
        // Only the pack emit branch scales pixels; the Record path never does.
        if (p[keys::kGain].is_number()) gain_ = clamp_gain_(p[keys::kGain].as_double());
        return true;
    }

    std::string exchange(const std::string& cmd_json) override {
        auto p = xi::Json::parse(cmd_json);
        const std::string command = p[keys::kCommand].as_string();

        if (command == keys::kStart)      { start_(); return get_def(); }
        if (command == keys::kStop)       { stop_();  return get_def(); }
        if (command == keys::kGetStatus)  { return get_def(); }
        if (command == keys::kSetFps) {
            // Guard 2: the payload is required — fail loud, don't silently no-op.
            auto v = p[keys::kValue];
            if (!v.valid())     return xi::contract::fault_json(xi::contract::kMissingInput, keys::kValue, "int");
            if (!v.is_number()) return xi::contract::fault_json(xi::contract::kWrongType,   keys::kValue, "int");
            fps_ = clamp_fps_(v.as_int());
            return get_def();
        }
        if (command == keys::kSetResolution) {
            auto w = p[keys::kWidth], h = p[keys::kHeight];
            if (!w.valid())     return xi::contract::fault_json(xi::contract::kMissingInput, keys::kWidth,  "int");
            if (!h.valid())     return xi::contract::fault_json(xi::contract::kMissingInput, keys::kHeight, "int");
            if (!w.is_number()) return xi::contract::fault_json(xi::contract::kWrongType,    keys::kWidth,  "int");
            if (!h.is_number()) return xi::contract::fault_json(xi::contract::kWrongType,    keys::kHeight, "int");
            if (w.as_int() > 0) w_ = w.as_int();
            if (h.as_int() > 0) h_ = h.as_int();
            return get_def();
        }
        if (command == keys::kSetGain) {
            // Same fail-loud contract as set_fps: the payload is required.
            auto v = p[keys::kValue];
            if (!v.valid())     return xi::contract::fault_json(xi::contract::kMissingInput, keys::kValue, "double");
            if (!v.is_number()) return xi::contract::fault_json(xi::contract::kWrongType,   keys::kValue, "double");
            gain_ = clamp_gain_(v.as_double());
            return get_def();
        }
        return exchange_unknown_command(command);
    }

    // ---- pack-door CONSUMER (polaris2 ex-feedback) --------------------------
    //
    // mock_camera EMITS pack frames (run_loop below) and ACCEPTS control packs
    // through this same xi.pack@1 door — the closed-loop pattern (analysis in the
    // script, actuation back into the source, effective on the NEXT emitted frame:
    // frame-latency control, examples/qa_pack_feedback). The door speaks the same
    // command vocabulary as exchange(): {command:"set_gain", value:f64}.
    void process(xi::PackIn& in, xi::PackOut& out) override {
        auto cmd = in.str(keys::kCommand);
        if (!cmd) {
            out.fault(xi::contract::kMissingInput, keys::kCommand,
                      "str (door command selector: set_gain | get_status)");
            return;
        }
        if (*cmd == keys::kSetGain) {
            if (!in.has(keys::kValue)) {
                out.fault(xi::contract::kMissingInput, keys::kValue, "f64 (brightness gain)");
                return;
            }
            auto v = in.f64(keys::kValue);
            if (!v) { if (auto iv = in.i64(keys::kValue)) v = (double)*iv; }
            if (!v) {
                out.fault(xi::contract::kWrongType, keys::kValue, "f64 (brightness gain)");
                return;
            }
            const double g = clamp_gain_(*v);
            gain_.store(g);
            // The ack: the sealed door output IS the reply. `seq` is the frame
            // counter at ack time — the new gain is in effect for every frame
            // painted after this store (i.e. no later than seq+1).
            out.str(keys::kAck, keys::kSetGain);
            out.f64(keys::kGain, g);
            out.i64(keys::kSeq, seq_.load());
            return;
        }
        if (*cmd == keys::kGetStatus) {
            out.str(keys::kAck, keys::kGetStatus);
            out.f64(keys::kGain, gain_.load());
            out.i64(keys::kSeq, seq_.load());
            out.boolean(keys::kStreaming, running_.load());
            return;
        }
        out.fault("unknown_command", keys::kCommand, *cmd);
    }

private:
    static int clamp_fps_(int v) { return v < 1 ? 1 : (v > 60 ? 60 : v); }
    static double clamp_gain_(double v) { return v < 0.05 ? 0.05 : (v > 8.0 ? 8.0 : v); }

    // Config touched from the dispatch/UI thread (set_def/exchange) and read from
    // the capture worker (run_loop) — atomic so those cross-thread reads are sound.
    // gain_ is additionally written by the pack DOOR (host-driven, another thread).
    std::atomic<int> w_{640}, h_{480}, fps_{10};
    std::atomic<bool> running_{false};
    std::atomic<bool> pack_mode_{false};   // polaris2 wave-2 (default OFF)
    std::atomic<double> gain_{1.0};        // polaris2 ex-feedback (pack-mode only)
    std::atomic<int> seq_{0};              // frame counter (door acks read it)
    std::thread thread_;

    void start_() {
        if (running_.load()) return;
        running_ = true;
        // Blessed source thread: xi::spawn_worker installs the per-thread SEH
        // translator + a top-level catch, so a stray fault in run_loop() is
        // contained to this worker instead of killing the whole backend (a raw
        // std::thread would run OUTSIDE that translator).
        thread_ = xi::spawn_worker(name() + "-source", [this] { run_loop(); });
    }

    void stop_() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    void run_loop() {
        seq_.store(0);
        while (running_) {
            const int seq = seq_.load();
            // Snapshot config once per frame (set_def/exchange may retune it live).
            const int w = w_.load(), h = h_.load(), fps = fps_.load();

            // Paint straight into a fresh host-pool slot: emit() then hands the
            // frame over via the pool refcount path (zero heap-to-pool copy).
            xi::Image img = pool_image(w, h, 3);
            uint8_t* p = img.data();

            // Background: gradient that shifts with frame
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    int i = (y * w + x) * 3;
                    p[i + 0] = static_cast<uint8_t>((x * 200 / w + seq * 2) & 0xFF);
                    p[i + 1] = static_cast<uint8_t>((y * 180 / h + seq * 3) & 0xFF);
                    p[i + 2] = static_cast<uint8_t>(80 + (seq & 0x3F));
                }
            }

            // Draw frame counter in top-left
            // Black background box
            for (int y = 2; y < 20; ++y) {
                for (int x = 2; x < 80; ++x) {
                    if (x < w && y < h) {
                        int i = (y * w + x) * 3;
                        p[i] = p[i+1] = p[i+2] = 0;
                    }
                }
            }
            draw_number(img, 4, 4, seq);

            // v12: the sealed xi.pack@1 Pack is the sole data plane. Emit the
            // frame's pixels (adopt_image is a zero-copy pool-handle addref, not a
            // memcpy) plus a "seq" entry and the gain it was painted with.
            //
            // ex-feedback: apply the door-controlled brightness gain to THIS frame
            // (saturating multiply; 1.0 is the identity). The pack ECHOES the gain
            // so a closed-loop script controls against the actual per-frame plant
            // state.
            const double g = gain_.load();
            if (g != 1.0) {
                const size_t n = (size_t)w * (size_t)h * 3;
                for (size_t i = 0; i < n; ++i) {
                    const double v = p[i] * g;
                    p[i] = v >= 255.0 ? (uint8_t)255 : (uint8_t)(v + 0.5);
                }
            }
            xi::PackOut f = new_pack();
            f.i64(keys::kSeq, seq);
            f.f64(keys::kGain, g);
            f.adopt_image(keys::kFrame, w, h, 3, img.pool_handle());
            emit(std::move(f));
            seq_.fetch_add(1);

            int sleep_ms = 1000 / std::max(fps, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }
};

XI_PLUGIN_IMPL(MockCamera)

// polaris2 ex-feedback: publish the control door (xi.pack@1 consumer). This is
// what lets a script close the loop on the camera itself:
// xi::use("cam").process(ctrl) / .push(ctrl). Additive — a host that never
// probes xi_plugin_get_interface sees exactly the plugin it always saw.
XI_PLUGIN_PACK_DOOR(MockCamera)
