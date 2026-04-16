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

#include <xi/xi_instance.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_source.hpp>

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

class MockCamera : public xi::ImageSource {
public:
    explicit MockCamera(std::string name)
        : ImageSource(std::move(name), 3), w_(640), h_(480), fps_(10) {}

    ~MockCamera() override { stop(); }

    std::string plugin_name() const override { return "mock_camera"; }

    std::string get_def() const override {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            R"({"width":%d,"height":%d,"fps":%d,"streaming":%s})",
            w_, h_, fps_, is_running() ? "true" : "false");
        return buf;
    }

    bool set_def(const std::string& j) override {
        auto extract_int = [&](const char* key) -> int {
            auto k = std::string("\"") + key + "\":";
            auto pos = j.find(k);
            if (pos == std::string::npos) return -1;
            return std::stoi(j.substr(pos + k.size()));
        };
        int v;
        if ((v = extract_int("width"))  > 0) w_ = v;
        if ((v = extract_int("height")) > 0) h_ = v;
        if ((v = extract_int("fps"))    > 0) fps_ = v;
        return true;
    }

    std::string exchange(const std::string& cmd_json) override {
        if (cmd_json.find("\"start\"") != std::string::npos) {
            start();
            return get_def();
        }
        if (cmd_json.find("\"stop\"") != std::string::npos) {
            stop();
            return get_def();
        }
        if (cmd_json.find("\"get_status\"") != std::string::npos) {
            return get_def();
        }
        if (cmd_json.find("\"set_fps\"") != std::string::npos) {
            auto pos = cmd_json.find("\"value\":");
            if (pos != std::string::npos) {
                fps_ = std::stoi(cmd_json.substr(pos + 8));
                if (fps_ < 1) fps_ = 1;
                if (fps_ > 60) fps_ = 60;
            }
            return get_def();
        }
        if (cmd_json.find("\"set_resolution\"") != std::string::npos) {
            set_def(cmd_json);
            return get_def();
        }
        return R"({"error":"unknown command"})";
    }

    void start() override {
        if (running_) return;
        running_ = true;
        thread_ = std::thread([this] { run_loop(); });
    }

    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

private:
    int w_, h_, fps_;
    std::thread thread_;

    void run_loop() {
        int seq = 0;
        while (running_) {
            xi::Image img(w_, h_, 3);
            uint8_t* p = img.data();

            // Background: gradient that shifts with frame
            for (int y = 0; y < h_; ++y) {
                for (int x = 0; x < w_; ++x) {
                    int i = (y * w_ + x) * 3;
                    p[i + 0] = static_cast<uint8_t>((x * 200 / w_ + seq * 2) & 0xFF);
                    p[i + 1] = static_cast<uint8_t>((y * 180 / h_ + seq * 3) & 0xFF);
                    p[i + 2] = static_cast<uint8_t>(80 + (seq & 0x3F));
                }
            }

            // Draw frame counter in top-left
            // Black background box
            for (int y = 2; y < 20; ++y) {
                for (int x = 2; x < 80; ++x) {
                    if (x < w_ && y < h_) {
                        int i = (y * w_ + x) * 3;
                        p[i] = p[i+1] = p[i+2] = 0;
                    }
                }
            }
            draw_number(img, 4, 4, seq);

            push(std::move(img));
            seq++;

            int sleep_ms = 1000 / std::max(fps_, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }
};

extern "C" __declspec(dllexport)
xi::InstanceBase* xi_plugin_create(const char* instance_name) {
    return new MockCamera(instance_name);
}
