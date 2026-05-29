#pragma once
//
// xi_crash_report.hpp — portable crash-report parser for the FE supervisor.
//
// The FE supervisor (fe_main.cpp) reads the backend's own log to find the
// absolute minidump path it printed on death, then reads the sibling .json
// crash report for forensics (exception name/module, last_phase) that it hands
// to the SafeStateSink. That logic lived as a FILE-STATIC function inside
// fe_main.cpp (enrich_from_crash_report) and so could not be unit-tested in
// isolation.
//
// This header lifts it out verbatim (same regex, same last-match-wins, same
// threads[] fallback that prefers the "inspect" breadcrumb) into a portable,
// header-only function so it can be exercised by a C++ unit (test_qa_edge.cpp)
// against crafted be.log + .json fixtures.
//
// Deliberately portable: only <string>/<fstream>/<sstream>/<regex>/<filesystem>
// + cJSON (already a backend dep). No Win32, so it compiles unchanged on Linux
// (see docs/design/linux-port.md). The FE simply forwards its &SafeStateEvent.
//
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <cJSON.h>

#include <xi/xi_safe_state.hpp>

namespace xi {

// Parse the BE's log for the LAST "minidump: <path>.dmp" line it printed, then
// read the sibling .json crash report and fill ev.{report_path, exception_name,
// faulting_module, last_phase}. Best-effort and noexcept-by-contract: any
// missing/corrupt input leaves the corresponding fields untouched and returns
// without throwing.
//
//   - no be.log / unreadable          -> returns, fields untouched
//   - no "minidump:" line             -> returns, report_path untouched
//   - multiple "minidump:" lines      -> the LAST wins (most recent crash)
//   - sibling .json missing/corrupt   -> report_path set, other fields untouched
//   - context.last_phase empty        -> falls back to threads[]; prefers "inspect"
//
inline void enrich_from_crash_report(const std::string& be_log, SafeStateEvent& ev) {
    std::ifstream f(be_log);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    std::string text = ss.str();

    // BE prints: "... — minidump: C:\...\xinsp-backend-<pid>-<ts>.dmp"
    std::regex re(R"(minidump:\s*(.+?\.dmp))");
    std::string dmp;
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
        dmp = (*it)[1].str();   // keep the last match
    if (dmp.empty()) return;
    // Trim trailing whitespace/CR.
    while (!dmp.empty() && (dmp.back() == '\r' || dmp.back() == '\n' || dmp.back() == ' '))
        dmp.pop_back();

    std::filesystem::path json_path =
        std::filesystem::path(dmp).replace_extension(".json");
    ev.report_path = json_path.string();
    std::ifstream jf(json_path);
    if (!jf) return;
    std::stringstream js; js << jf.rdbuf();
    cJSON* root = cJSON_Parse(js.str().c_str());
    if (!root) return;
    if (cJSON* exc = cJSON_GetObjectItem(root, "exception")) {
        if (cJSON* nm = cJSON_GetObjectItem(exc, "name"); cJSON_IsString(nm))
            ev.exception_name = nm->valuestring;
        if (cJSON* md = cJSON_GetObjectItem(exc, "module"); cJSON_IsString(md))
            ev.faulting_module = md->valuestring;
    }
    if (cJSON* ctx = cJSON_GetObjectItem(root, "context")) {
        if (cJSON* ph = cJSON_GetObjectItem(ctx, "last_phase"); cJSON_IsString(ph))
            ev.last_phase = ph->valuestring;
    }
    // If context didn't name a phase (crash on an unmanaged thread), fall back
    // to the most informative thread breadcrumb in threads[].
    if (ev.last_phase.empty()) {
        if (cJSON* th = cJSON_GetObjectItem(root, "threads"); cJSON_IsArray(th)) {
            cJSON* t = nullptr;
            cJSON_ArrayForEach(t, th) {
                cJSON* ph = cJSON_GetObjectItem(t, "last_phase");
                if (cJSON_IsString(ph) && ph->valuestring && ph->valuestring[0]) {
                    ev.last_phase = ph->valuestring;
                    if (std::string(ph->valuestring) == "inspect") break;  // prefer inspect
                }
            }
        }
    }
    cJSON_Delete(root);
}

} // namespace xi
