//
// crash_then_heal.cpp — crashes the backend its first N starts, then RECOVERS.
//
// Phase G (#92) needs to exercise the FE supervisor's recover-and-clear
// transition (test plan FE-E5 / safety property SP4): a line that was driven
// safe by a crash-loop must have its safe state CLEARED once the backend comes
// back healthy — and the FE must NOT hit the respawn cap if recovery happens
// first. fe_supervisor only proves the cap path (crash forever); this proves the
// happy exit from safe state.
//
// Mechanism: a counter file that survives the backend dying + respawning (the
// crash takes the whole process down, so in-memory state is useless). On each
// process(), read the count; while it is below `crash_count`, bump the file and
// crash UNCATCHABLY (raw std::thread null-deref, exactly like raw_thread_crash —
// outside the dispatch-thread SEH translator). Once the count reaches the
// threshold, return normally forever => the backend stays up => the FE clears.
//
// The counter path comes from XI_QA_RECOVER_MARKER (the driver sets it and
// inherits it down FE -> BE -> plugin); falls back to a cwd-relative name.
//
#include <xi/xi.hpp>
#include <xi/xi_json.hpp>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <string>

class CrashThenHeal : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    void process(xi::PackIn& /*in*/, xi::PackOut& out) override {
        long count = read_count();
        if (count < crash_count_) {
            write_count(count + 1);   // persist the crash BEFORE we die
            // Uncatchable on purpose (see file header / raw_thread_crash).
            std::thread t([] { *(volatile int*)nullptr = 0xDEAD; });
            t.join();                 // never returns: tears the process down
        }
        // Healed: never crash again. `recovered=1` lets the script/driver see it.
        out.i64("count", ++frames_).i64("recovered", 1);
    }

    std::string exchange(const std::string& /*cmd*/) override { return get_def(); }

    std::string get_def() const override {
        return xi::Json::object()
            .set("crash_count", (int)crash_count_)
            .set("frames",      frames_)
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        crash_count_ = p["crash_count"].as_int((int)crash_count_);
        return true;
    }

private:
    std::string marker_path() const {
        const char* m = std::getenv("XI_QA_RECOVER_MARKER");
        return (m && *m) ? std::string(m) : std::string("xinsp2_qa_recover.marker");
    }
    long read_count() const {
        std::FILE* f = std::fopen(marker_path().c_str(), "r");
        if (!f) return 0;
        long v = 0;
        if (std::fscanf(f, "%ld", &v) != 1) v = 0;
        std::fclose(f);
        return v;
    }
    void write_count(long v) const {
        std::FILE* f = std::fopen(marker_path().c_str(), "w");
        if (f) { std::fprintf(f, "%ld", v); std::fclose(f); }   // fclose flushes
    }

    long crash_count_ = 2;
    int  frames_ = 0;
};

XI_PLUGIN_IMPL(CrashThenHeal)
XI_PLUGIN_PACK_DOOR(CrashThenHeal)
