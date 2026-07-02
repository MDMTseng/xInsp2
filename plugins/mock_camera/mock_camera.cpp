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

#include <xi/xi_abi.hpp>       // xi::Plugin, xi::Record, xi::Image, pool_image()/emit()
#include <xi/xi_thread.hpp>    // xi::spawn_worker — SEH-safe capture thread
#include <xi/xi_json.hpp>      // parses set_def/exchange (canonical over cmd.find)
#include <xi/xi_contract.hpp>  // fail-loud command inputs + schema-skew errors

#include "mock_camera_keys.h"

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
        return exchange_unknown_command(command);
    }

private:
    static int clamp_fps_(int v) { return v < 1 ? 1 : (v > 60 ? 60 : v); }

    // Config touched from the dispatch/UI thread (set_def/exchange) and read from
    // the capture worker (run_loop) — atomic so those cross-thread reads are sound.
    std::atomic<int> w_{640}, h_{480}, fps_{10};
    std::atomic<bool> running_{false};
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
        int seq = 0;
        while (running_) {
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

            // emit() fills host()/name(), mints a fresh trigger id, and runs the
            // RAII marshal/refcount path — the member sibling of the free
            // xi::emit_record, no manual host_-> juggling.
            xi::Record rec;
            rec.image(keys::kFrame, img);
            emit(rec);
            seq++;

            int sleep_ms = 1000 / std::max(fps, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }
};

XI_PLUGIN_IMPL(MockCamera)
