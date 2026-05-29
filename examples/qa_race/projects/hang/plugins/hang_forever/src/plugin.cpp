//
// hang_forever.cpp — process() blocks forever. Fixture for the FE-E4
// PortUnresponsive DESIGN (see qa_race/PLAN.md). The infinite sleep wedges a
// dispatch thread; it does NOT (by itself) stop the WS accept loop, because
// inspects run on detached threads in the backend. RACE-FE4 documents what an
// additional BE poll-stall hook would need to do to make the probe fail.
//
#include <xi/xi.hpp>
#include <xi/xi_json.hpp>
#include <chrono>
#include <thread>

class HangForever : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& /*input*/) override {
        // Block this dispatch thread indefinitely. Poll cancellation so a
        // watchdog / TerminateProcess still reaps us promptly.
        while (!xi::cancellation_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return xi::Record().set("count", -1);
    }

    std::string get_def() const override {
        return xi::Json::object().dump();
    }

    bool set_def(const std::string& /*json*/) override { return true; }
};

XI_PLUGIN_IMPL(HangForever)
