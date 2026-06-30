#pragma once
//
// xi_crash_history.hpp — append-only structured BE-death log for the FE supervisor.
//
// The FE (xinsp-fe.exe) is the only process that survives every backend death,
// so it is the natural place to keep a *structured* crash history. The BE writes
// the minidump + JSON crash report itself (it is the crashing process; dbghelp
// lives there, not here — the FE stays dependency-light and never writes dumps).
// The FE's job is the timeline: each BE death appends one JSON line (JSONL) here
// with the death reason, exit code, parsed crash forensics, the consecutive-
// failure count, and whether the respawn cap was hit. One file = the whole
// crash-loop story at a glance, and a feed a future UI / FE status channel can read.
//
// Optional dump preservation: the BE drops dumps in %TEMP% where a temp cleanup
// or a crash-loop can overwrite/lose them. With a preserve dir set, the FE copies
// the referenced .dmp (+ sibling .json) somewhere stable as it records the death.
//
// Deliberately portable: only <string>/<fstream>/<filesystem>/<cstdio>. No Win32,
// no OpenCV — compiles unchanged on Linux (see docs/roadmap/linux-port.md). The
// JSONL line builder (crash_history_line) is pure so it can be unit-tested.
//
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <xi/xi_safe_state.hpp>

namespace xi {

// JSON-escape a string for embedding as a JSONL value. Handles the cases that
// actually occur here: backslashes in Windows paths, quotes, and control chars.
inline std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

// Build the JSONL record (no trailing newline) for one BE death. Pure + testable.
//   consecutive: the consecutive-failure count AFTER this death is counted.
//   cap_hit:     true if this death tripped the respawn cap (line latches safe).
inline std::string crash_history_line(const SafeStateEvent& ev,
                                      int consecutive, bool cap_hit) {
    std::string o = "{";
    o += "\"ts_ms\":" + std::to_string(static_cast<long long>(ev.ts_ms));
    o += ",\"reason\":\"" + std::string(to_string(ev.reason)) + "\"";
    o += ",\"rc\":" + std::to_string(ev.backend_rc);
    o += ",\"exception\":\"" + json_escape(ev.exception_name) + "\"";
    o += ",\"module\":\""    + json_escape(ev.faulting_module) + "\"";
    o += ",\"phase\":\""     + json_escape(ev.last_phase) + "\"";
    o += ",\"report\":\""    + json_escape(ev.report_path) + "\"";
    o += ",\"dump\":\""      + json_escape(ev.dump_path) + "\"";
    // Part III G2.1 — which plugin the death was attributed to (empty = not
    // attributable to a plugin). The whole crash-loop-per-plugin story is now
    // visible in the JSONL, and `culprit_plugin` is what the per-plugin counter
    // (xi_respawn_policy.hpp PluginFaultTracker) keys on.
    o += ",\"culprit_plugin\":\""   + json_escape(ev.culprit_plugin)   + "\"";
    o += ",\"culprit_instance\":\"" + json_escape(ev.culprit_instance) + "\"";
    o += ",\"consecutive\":" + std::to_string(consecutive);
    o += ",\"cap_hit\":" + std::string(cap_hit ? "true" : "false");
    o += "}";
    return o;
}

// Append-only crash history (+ optional dump preservation). All I/O is
// best-effort and never throws: a failure to write the history must not take the
// supervisor down (its first duty is the line's safe state, not its own logging).
class CrashHistory {
public:
    // path:         JSONL file to append to. Empty -> disabled (record() is a no-op).
    // preserve_dir: if non-empty, copy the BE's .dmp (+ sibling .json) here on each
    //               record so a %TEMP% cleanup / crash-loop overwrite can't lose it.
    // max_bytes:    rotate the JSONL (keep one .1 generation) once it exceeds this,
    //               so a chronically flapping line can't fill the disk over weeks.
    // max_preserved: cap on files kept in preserve_dir; oldest are pruned. Each
    //               crash copies a .dmp + .json pair, so the dir is bounded too.
    explicit CrashHistory(std::string path, std::string preserve_dir = {},
                          std::uintmax_t max_bytes = 4u * 1024 * 1024,
                          std::size_t max_preserved = 100)
        : path_(std::move(path)), preserve_dir_(std::move(preserve_dir)),
          max_bytes_(max_bytes), max_preserved_(max_preserved) {}

    bool enabled() const { return !path_.empty(); }

    void record(const SafeStateEvent& ev, int consecutive, bool cap_hit) {
        if (path_.empty()) return;
        try {
            rotate_if_needed_();
            std::ofstream f(path_, std::ios::app);
            if (f) f << crash_history_line(ev, consecutive, cap_hit) << "\n";
        } catch (...) { /* best-effort */ }
        if (!preserve_dir_.empty() && !ev.dump_path.empty()) {
            preserve_dump_(ev.dump_path);
            prune_preserved_();
        }
    }

private:
    // Roll path_ -> path_.1 (overwriting any prior .1) once it grows past max_bytes_,
    // so at most ~2*max_bytes_ of history exists. Keeps the most recent generation
    // intact for forensics while bounding total size.
    void rotate_if_needed_() {
        if (max_bytes_ == 0) return;
        std::error_code ec;
        auto sz = std::filesystem::file_size(path_, ec);
        if (ec || sz < max_bytes_) return;
        std::filesystem::rename(path_, path_ + ".1", ec);  // overwrites prior .1
    }

    // Keep only the newest max_preserved_ entries in preserve_dir_ (by last-write
    // time), deleting the oldest. Bounds the dump directory on a flapping line.
    void prune_preserved_() {
        if (max_preserved_ == 0) return;
        std::error_code ec;
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
        for (auto& e : std::filesystem::directory_iterator(preserve_dir_, ec)) {
            if (ec) return;
            if (!e.is_regular_file(ec)) continue;
            auto t = std::filesystem::last_write_time(e.path(), ec);
            if (!ec) files.emplace_back(t, e.path());
        }
        if (files.size() <= max_preserved_) return;
        std::sort(files.begin(), files.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t i = 0; i + max_preserved_ < files.size(); ++i)
            std::filesystem::remove(files[i].second, ec);
    }

    void preserve_dump_(const std::string& dmp) {
        std::error_code ec;
        std::filesystem::path src(dmp);
        if (!std::filesystem::exists(src, ec)) return;
        std::filesystem::create_directories(preserve_dir_, ec);
        const auto opt = std::filesystem::copy_options::overwrite_existing;
        std::filesystem::copy_file(src, std::filesystem::path(preserve_dir_) / src.filename(), opt, ec);
        // Pull the sibling .json report alongside the dump if present.
        std::filesystem::path jsrc = src;
        jsrc.replace_extension(".json");
        if (std::filesystem::exists(jsrc, ec))
            std::filesystem::copy_file(jsrc, std::filesystem::path(preserve_dir_) / jsrc.filename(), opt, ec);
    }

    std::string path_;
    std::string preserve_dir_;
    std::uintmax_t max_bytes_;
    std::size_t max_preserved_;
};

} // namespace xi
