//
// crash_once_heal.cpp — crashes the backend on the FIRST process() of a fresh
// on-disk instance, then runs HEALTHY on every subsequent backend start.
//
// This is the deterministic fixture for FE-E5 (recover-and-clear): a project
// that "crashes once then heals after a respawn". The trick is that the
// crash decision must survive a backend RESTART (each respawn is a brand-new
// process that re-opens the project and re-instantiates this plugin), so an
// in-memory flag is useless. Instead we use a MARKER FILE in the instance's
// own on-disk folder (xi::Plugin::folder_path() -> project/instances/<name>/):
//
//   process():
//     if marker file absent  -> write it, then crash UNCATCHABLY (raw thread
//                               null-deref, same mechanism as raw_thread_crash;
//                               see that plugin for why a bare std::thread
//                               escapes the host's per-dispatch-thread SEH
//                               translator straight to the top-level
//                               SetUnhandledExceptionFilter -> minidump).
//     if marker file present -> heal: return a normal count, never crash.
//
// So: BE start #1 -> first inspect -> marker written -> crash -> BE dies.
// FE drives safe-state, respawns. BE start #2 -> open_project -> first inspect
// -> marker already there -> healthy forever. FE sees the port healthy and
// drives CLEAR SAFE STATE exactly once. No respawn cap.
//
// The driver deletes the marker before each run so the test is repeatable.
//
#include <xi/xi.hpp>
#include <xi/xi_json.hpp>

#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

class CrashOnceHeal : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& /*input*/) override {
        fs::path marker = marker_path();
        std::error_code ec;
        bool armed = !marker.empty() && !fs::exists(marker, ec);

        if (armed) {
            // Persist the "I have crashed once" marker BEFORE crashing, so the
            // next backend instance heals. Flush + close so it survives the
            // process teardown that follows immediately.
            if (!marker.empty()) {
                fs::create_directories(marker.parent_path(), ec);
                std::ofstream(marker.string(), std::ios::trunc) << "crashed-once\n";
            }
            // Uncatchable on purpose (raw thread, no xi::spawn_worker / no SEH
            // translator). Volatile store so the optimiser can't elide it.
            std::thread t([] { *(volatile int*)nullptr = 0xDEAD; });
            t.join();  // never returns; the faulting thread tears the process down
        }
        return xi::Record().set("count", ++frames_processed_)
                           .set("healed", true);
    }

    // Pack door (polaris2 THE CUT): pack-in/pack-out mirror of the Record door —
    // same crash-once-then-heal behaviour, same {count, healed} output.
    void process(xi::PackIn& /*in*/, xi::PackOut& out) override {
        fs::path marker = marker_path();
        std::error_code ec;
        bool armed = !marker.empty() && !fs::exists(marker, ec);
        if (armed) {
            if (!marker.empty()) {
                fs::create_directories(marker.parent_path(), ec);
                std::ofstream(marker.string(), std::ios::trunc) << "crashed-once\n";
            }
            std::thread t([] { *(volatile int*)nullptr = 0xDEAD; });
            t.join();
        }
        out.i64("count", ++frames_processed_).boolean("healed", true);
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("frames_processed", frames_processed_)
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        return p.valid();
    }

private:
    fs::path marker_path() const {
        std::string folder = folder_path();   // project/instances/<name>/
        if (folder.empty()) return {};
        return fs::path(folder) / ".crashed_once.marker";
    }

    int frames_processed_ = 0;
};

XI_PLUGIN_IMPL(CrashOnceHeal)
XI_PLUGIN_PACK_DOOR(CrashOnceHeal)
