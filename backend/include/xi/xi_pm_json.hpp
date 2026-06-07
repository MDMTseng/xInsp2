#pragma once
//
// xi_pm_json.hpp — minimal JSON-output helpers shared by the plugin/project
// machinery. Extracted from xi_plugin_manager.hpp so the escape primitive lives
// in one leaf header (it's the same shape as xp::json_escape_into in
// xi_protocol.hpp, duplicated here so this layer doesn't pull in the protocol
// parser as a transitive dependency).
//
// Header-only, std lib only — no platform coupling.
//
#include <cstdio>
#include <string>

namespace xi {

// Append a JSON-quoted, escaped form of `s` to `out`. Covers the minimal RFC
// 8259 requirements (\" \\ control chars as \uXXXX), so a name containing a
// quote or control char can't corrupt project.json / plugin metadata.
inline void pm_json_escape(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x",
                                  (unsigned)(unsigned char)c);
                    out += b;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

// Convenience: returns a fresh quoted+escaped string.
inline std::string pm_json_quote(const std::string& s) {
    std::string out;
    pm_json_escape(out, s);
    return out;
}

} // namespace xi
