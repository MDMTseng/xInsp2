// light_classifier.cpp — cheap downstream plugin for burst_pipeline.
//
// process() reads the seq stamped in the first 8 bytes of the input
// image, sleeps ~5 ms (stand-in for real but cheap CV work), and
// returns { seq, kind = seq % 3, processed = atomic counter }.
//
// MUST be reentrant — burst_pipeline's inspect runs under
// dispatch_threads up to 4. Only state is the std::atomic processed
// counter, which is reentrancy-safe by construction.
//
// manifest.thread_safe = true (currently informational; backend
// doesn't enforce it but it's the contract).

#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

class LightClassifier : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        int sleep_ms = input["sleep_ms"].as_int(5);
        if (sleep_ms < 0)    sleep_ms = 0;
        if (sleep_ms > 1000) sleep_ms = 1000;

        // Decode embedded seq.
        uint64_t seq = 0;
        const auto& img = input.get_image("img");
        if (!img.empty() && img.data() != nullptr) {
            // First 8 bytes of pixel data = little-endian uint64 seq.
            std::memcpy(&seq, img.data(), sizeof(seq));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));

        int n = ++processed_;
        int kind = (int)(seq % 3);

        return xi::Record()
            .set("seq",       (int)(seq & 0x7fffffff))
            .set("kind",      kind)
            .set("processed", n);
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("processed", processed_.load())
            .dump();
    }
    bool set_def(const std::string&) override { return true; }

private:
    std::atomic<int> processed_{0};
};

XI_PLUGIN_IMPL(LightClassifier)
