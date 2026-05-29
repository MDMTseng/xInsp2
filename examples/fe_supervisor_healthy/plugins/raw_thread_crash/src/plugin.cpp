//
// raw_thread_crash.cpp — deliberately takes the backend down in an
// UNCATCHABLE way, to exercise the crash-forensics path that replaced
// process isolation under the FE/BE in-process split.
//
// Why a raw std::thread: the host installs `_set_se_translator` on every
// dispatch thread and wraps each plugin call in try/catch(seh_exception),
// so an access violation on the dispatch thread is converted to a C++
// throw and absorbed (the backend keeps running). A thread the plugin
// spawns itself with a bare `std::thread` has NO translator and sits
// outside that try/catch, so its access violation propagates to the
// host's top-level SetUnhandledExceptionFilter -> MiniDumpWriteDump +
// JSON crash report. This is the exact failure mode docs/guides/
// debugging.md calls out ("plugin worker thread without xi::spawn_worker").
//
// The point of the test: even though the whole backend dies, the crash
// report's threads[] array still carries the DISPATCH thread's breadcrumb
// (last_cmd="inspect", last_phase="inspect", last_run_id/last_frame), and
// the minidump captures the faulting stack — so an operator can debug it.
//
#include <xi/xi.hpp>
#include <xi/xi_json.hpp>
#include <thread>

class RawThreadCrash : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& /*input*/) override {
        if (armed_) {
            // Uncatchable on purpose — see file header. Volatile store so
            // the optimiser can't elide the null write.
            std::thread t([] { *(volatile int*)nullptr = 0xDEAD; });
            t.join();  // never returns: the faulting thread tears the process down
        }
        return xi::Record().set("count", ++frames_processed_);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "arm")        armed_ = true;
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
