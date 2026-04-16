#pragma once
//
// xi_source.hpp — image source with trigger-based execution.
//
// An ImageSource is an Instance that produces frames. When a frame arrives,
// it triggers the inspection pipeline. The user script calls grab() to
// dequeue the latest frame.
//
// Usage in a user script:
//
//   xi::Instance<TestImageSource> cam{"cam0"};
//
//   void xi_inspect_entry(int frame) {
//       VAR(img, cam->grab());        // dequeues latest
//       VAR(gray, toGray(img));
//       ...
//   }
//
// The backend service registers a trigger callback on the source. When
// the source pushes a frame, the callback fires, and the service schedules
// an inspect() call on its worker thread.
//

#include "xi_image.hpp"
#include "xi_instance.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace xi {

class ImageSource : public InstanceBase {
public:
    using TriggerCallback = std::function<void()>;

    explicit ImageSource(std::string name, int max_queue = 3)
        : name_(std::move(name)), max_queue_(max_queue) {}

    ~ImageSource() override { stop(); }

    const std::string& name() const override { return name_; }
    std::string plugin_name() const override { return "ImageSource"; }

    // --- Producer side (called from acquisition thread) ---

    void push(Image img) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.push_back(std::move(img));
            while ((int)queue_.size() > max_queue_) {
                queue_.pop_front(); // drop oldest
            }
            frame_count_++;
        }
        cv_.notify_one();
        if (on_trigger_) on_trigger_();
    }

    // --- Consumer side (called from user script) ---

    // Grab the newest frame, discard older ones. Returns empty Image if
    // queue is empty (script should check .empty()).
    Image grab() {
        std::lock_guard<std::mutex> lk(mu_);
        if (queue_.empty()) return {};
        Image img = std::move(queue_.back());
        queue_.clear();
        return img;
    }

    // Blocking grab — waits up to timeout_ms for a frame.
    Image grab_wait(int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lk(mu_);
        if (queue_.empty()) {
            cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                         [this] { return !queue_.empty(); });
        }
        if (queue_.empty()) return {};
        Image img = std::move(queue_.back());
        queue_.clear();
        return img;
    }

    // --- Trigger registration (called by backend service) ---

    void set_trigger(TriggerCallback cb) { on_trigger_ = std::move(cb); }

    // --- Lifecycle ---

    virtual void start() { running_ = true; }
    virtual void stop()  { running_ = false; }
    bool is_running() const { return running_; }

    int64_t frame_count() const { return frame_count_; }

protected:
    std::string              name_;
    int                      max_queue_;
    std::mutex               mu_;
    std::condition_variable  cv_;
    std::deque<Image>        queue_;
    std::atomic<bool>        running_{false};
    std::atomic<int64_t>     frame_count_{0};
    TriggerCallback          on_trigger_;
};

// ---------- Synthetic test source ----------
//
// Generates frames at a fixed FPS with a pattern that varies per frame.
// Use this during development before real camera integration.

class TestImageSource : public ImageSource {
public:
    explicit TestImageSource(std::string name, int w = 640, int h = 480, int fps = 10)
        : ImageSource(std::move(name)), w_(w), h_(h), fps_(fps) {}

    ~TestImageSource() override { stop(); }

    std::string plugin_name() const override { return "TestImageSource"; }

    std::string get_def() const override {
        return "{\"width\":" + std::to_string(w_) +
               ",\"height\":" + std::to_string(h_) +
               ",\"fps\":" + std::to_string(fps_) + "}";
    }

    bool set_def(const std::string& j) override {
        auto extract = [&](const char* key) -> int {
            auto pos = j.find(std::string("\"") + key + "\":");
            if (pos == std::string::npos) return -1;
            return std::stoi(j.substr(pos + std::strlen(key) + 3));
        };
        int v;
        if ((v = extract("width"))  > 0) w_ = v;
        if ((v = extract("height")) > 0) h_ = v;
        if ((v = extract("fps"))    > 0) fps_ = v;
        return true;
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
            Image img(w_, h_, 3);
            uint8_t* p = img.data();
            for (int y = 0; y < h_; ++y) {
                for (int x = 0; x < w_; ++x) {
                    int i = (y * w_ + x) * 3;
                    p[i + 0] = static_cast<uint8_t>((x + seq * 3) & 0xFF);
                    p[i + 1] = static_cast<uint8_t>((y + seq * 5) & 0xFF);
                    p[i + 2] = static_cast<uint8_t>((x + y + seq) & 0xFF);
                }
            }
            push(std::move(img));
            seq++;

            int sleep_ms = 1000 / std::max(fps_, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }
};

// Factory specializations
template <>
inline std::shared_ptr<ImageSource> make_plugin_instance<ImageSource>(std::string_view name) {
    return std::make_shared<ImageSource>(std::string(name));
}

template <>
inline std::shared_ptr<TestImageSource> make_plugin_instance<TestImageSource>(std::string_view name) {
    return std::make_shared<TestImageSource>(std::string(name));
}

} // namespace xi
