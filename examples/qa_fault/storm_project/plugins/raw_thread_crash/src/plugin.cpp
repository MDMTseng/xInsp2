//
// raw_thread_crash.cpp — qa_fault copy. Crashes the backend uncatchably on
// every armed process() via a bare std::thread null-deref (no SEH translator
// on an unmanaged plugin thread -> escapes to the host's top-level
// SetUnhandledExceptionFilter -> minidump + crash report). Identical mechanism
// to examples/plugin_crash_forensics; copied here so qa_fault is self-contained
// and never disturbs another example. Used by QF-I7 (respawn-cap accounting).
//
#include <xi/xi.hpp>
#include <xi/xi_json.hpp>
#include <thread>

class RawThreadCrash : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& /*input*/) override {
        if (armed_) {
            std::thread t([] { *(volatile int*)nullptr = 0xDEAD; });
            t.join();  // never returns: the faulting thread tears the process down
        }
        return xi::Record().set("count", ++frames_processed_);
    }

    // Pack door (polaris2 THE CUT): pack-in/pack-out mirror of the Record door —
    // same crash-if-armed behaviour, same {count} output. Bilingual.
    void process(xi::PackIn& /*in*/, xi::PackOut& out) override {
        if (armed_) {
            std::thread t([] { *(volatile int*)nullptr = 0xDEAD; });
            t.join();
        }
        out.i64("count", ++frames_processed_);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "arm")         armed_ = true;
        else if (command == "disarm") armed_ = false;
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("armed",            armed_)
            .set("frames_processed", frames_processed_)
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        armed_ = p["armed"].as_bool(armed_);
        return true;
    }

private:
    bool armed_ = false;
    int  frames_processed_ = 0;
};

XI_PLUGIN_IMPL(RawThreadCrash)
XI_PLUGIN_PACK_DOOR(RawThreadCrash)
