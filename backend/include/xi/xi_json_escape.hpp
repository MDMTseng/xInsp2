#pragma once
//
// xi_json_escape.hpp — the ONE JSON string-escape primitive.
//
// Four headers used to carry byte-identical copies of this loop (xi_protocol's
// json_escape_into, xi_record's append_json_escaped, xi_param's Param::escape_json,
// xi_pm_json's pm_json_escape), each with a "kept local to avoid depending on the
// protocol parser" comment. This header IS that minimal dependency: std-lib only,
// no platform/protocol coupling — so every layer can route through it and stay a
// leaf. The old names survive as one-line forwarders; call sites are unchanged.
//
// Covers the minimal RFC 8259 requirements: wraps in quotes, escapes \" and \\,
// the named whitespace controls (\n \r \t), and every other control byte (< 0x20)
// as \uXXXX. A name/value containing a quote or control char can't corrupt the
// surrounding JSON (vars frame, project.json, plugin metadata).
//
#include <cstdio>
#include <string>
#include <string_view>

namespace xi {

inline void json_escape_into(std::string& out, std::string_view s) {
    out.reserve(out.size() + s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  (unsigned)(unsigned char)c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

// Convenience: returns a fresh quoted+escaped string.
inline std::string json_escape(std::string_view s) {
    std::string out;
    json_escape_into(out, s);
    return out;
}

} // namespace xi
