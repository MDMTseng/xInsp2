#pragma once
//
// xi_protocol.hpp — xInsp2 WebSocket protocol types (C++ side).
//
// Canonical schema lives in protocol/messages.md. This header mirrors it
// as plain C++ structs plus minimal JSON encode/decode helpers.
//
// This file deliberately avoids nlohmann/json (and any other dep) so the
// xi_core target stays header-only. The parser is small and strict — it
// accepts the JSON shapes this protocol produces, not arbitrary JSON. When
// M2 brings in a real parser, replace parse_* with nlohmann calls behind
// the same struct interface.
//

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace xi::proto {

// ---------- common enums ----------

enum class VarKindWire : uint8_t {
    Image   = 0,
    Number  = 1,
    Boolean = 2,
    String  = 3,
    Json    = 4,
    Custom  = 5,
};

inline const char* to_string(VarKindWire k) {
    switch (k) {
        case VarKindWire::Image:   return "image";
        case VarKindWire::Number:  return "number";
        case VarKindWire::Boolean: return "boolean";
        case VarKindWire::String:  return "string";
        case VarKindWire::Json:    return "json";
        case VarKindWire::Custom:  return "custom";
    }
    return "unknown";
}

inline std::optional<VarKindWire> parse_var_kind(std::string_view s) {
    if (s == "image")   return VarKindWire::Image;
    if (s == "number")  return VarKindWire::Number;
    if (s == "boolean") return VarKindWire::Boolean;
    if (s == "string")  return VarKindWire::String;
    if (s == "json")    return VarKindWire::Json;
    if (s == "custom")  return VarKindWire::Custom;
    return std::nullopt;
}

enum class Codec : uint32_t {
    JPEG = 0,
    BMP  = 1,
    PNG  = 2,
};

// ---------- binary preview header ----------

struct PreviewHeader {
    uint32_t gid;
    uint32_t codec;      // Codec
    uint32_t width;
    uint32_t height;
    uint32_t channels;
};

static constexpr size_t kPreviewHeaderSize = 20;

// Big-endian helpers — WebSocket is byte-oriented but we fix endianness so
// multiple clients (JS, Python, C++) agree.
inline void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8)  & 0xFF);
    p[3] = static_cast<uint8_t>( v        & 0xFF);
}
inline uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
            static_cast<uint32_t>(p[3]);
}

inline void encode_preview_header(const PreviewHeader& h, uint8_t out[kPreviewHeaderSize]) {
    write_u32_be(out + 0,  h.gid);
    write_u32_be(out + 4,  h.codec);
    write_u32_be(out + 8,  h.width);
    write_u32_be(out + 12, h.height);
    write_u32_be(out + 16, h.channels);
}

inline PreviewHeader decode_preview_header(const uint8_t in[kPreviewHeaderSize]) {
    PreviewHeader h;
    h.gid      = read_u32_be(in + 0);
    h.codec    = read_u32_be(in + 4);
    h.width    = read_u32_be(in + 8);
    h.height   = read_u32_be(in + 12);
    h.channels = read_u32_be(in + 16);
    return h;
}

// ---------- JSON string escape / unescape ----------

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
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

inline std::string json_escape(std::string_view s) {
    std::string out;
    json_escape_into(out, s);
    return out;
}

// ---------- messages ----------

struct Cmd {
    int64_t     id;
    std::string name;
    std::string args_json;   // raw JSON object string, "{}" if empty

    std::string to_json() const {
        std::string out = "{\"type\":\"cmd\",\"id\":";
        out += std::to_string(id);
        out += ",\"name\":";
        json_escape_into(out, name);
        out += ",\"args\":";
        out += args_json.empty() ? "{}" : args_json;
        out += "}";
        return out;
    }
};

struct Rsp {
    int64_t     id;
    bool        ok;
    std::string data_json;  // empty if no data
    std::string error;      // empty if ok

    std::string to_json() const {
        std::string out = "{\"type\":\"rsp\",\"id\":";
        out += std::to_string(id);
        out += ",\"ok\":";
        out += ok ? "true" : "false";
        if (ok && !data_json.empty()) {
            out += ",\"data\":";
            out += data_json;
        }
        if (!ok && !error.empty()) {
            out += ",\"error\":";
            json_escape_into(out, error);
        }
        out += "}";
        return out;
    }
};

struct VarItem {
    std::string  name;
    VarKindWire  kind;
    // One of these is populated depending on kind:
    std::string  value_json;    // number / json / custom (raw JSON)
    std::string  value_str;     // string kind
    bool         value_bool = false;
    uint32_t     gid  = 0;      // image kind
    bool         raw  = false;  // image kind

    std::string to_json() const {
        std::string out = "{\"name\":";
        json_escape_into(out, name);
        out += ",\"kind\":\"";
        out += to_string(kind);
        out += "\"";
        switch (kind) {
            case VarKindWire::Image:
                out += ",\"gid\":"; out += std::to_string(gid);
                out += ",\"raw\":"; out += raw ? "true" : "false";
                break;
            case VarKindWire::Number:
            case VarKindWire::Json:
            case VarKindWire::Custom:
                out += ",\"value\":";
                out += value_json.empty() ? "null" : value_json;
                break;
            case VarKindWire::String:
                out += ",\"value\":";
                json_escape_into(out, value_str);
                break;
            case VarKindWire::Boolean:
                out += ",\"value\":";
                out += value_bool ? "true" : "false";
                break;
        }
        out += "}";
        return out;
    }
};

struct Vars {
    int64_t                run_id;
    std::vector<VarItem>   items;

    std::string to_json() const {
        std::string out = "{\"type\":\"vars\",\"run_id\":";
        out += std::to_string(run_id);
        out += ",\"items\":[";
        for (size_t i = 0; i < items.size(); ++i) {
            if (i) out += ",";
            out += items[i].to_json();
        }
        out += "]}";
        return out;
    }
};

struct LogMsg {
    std::string level;
    std::string msg;
    double      ts = 0.0;

    std::string to_json() const {
        std::string out = "{\"type\":\"log\",\"level\":";
        json_escape_into(out, level);
        out += ",\"msg\":";
        json_escape_into(out, msg);
        if (ts != 0.0) {
            out += ",\"ts\":";
            out += std::to_string(ts);
        }
        out += "}";
        return out;
    }
};

struct Event {
    std::string name;
    std::string data_json;  // raw JSON object string, "{}" if empty

    std::string to_json() const {
        std::string out = "{\"type\":\"event\",\"name\":";
        json_escape_into(out, name);
        out += ",\"data\":";
        out += data_json.empty() ? "{}" : data_json;
        out += "}";
        return out;
    }
};

// ---------- minimal JSON cursor for parsing cmd messages ----------
//
// The protocol surface the backend needs to parse is small: `cmd` messages
// with `type`, `id`, `name`, `args`. We need to extract these four fields
// without pulling in a dep. This parser is deliberately minimal — it trusts
// well-formed input from our own clients and returns nullopt on anything
// unexpected. M2 replaces it with nlohmann/json.

struct ParsedCmd {
    int64_t     id   = 0;
    std::string name;
    std::string args_json;  // substring of the original input, including braces
};

namespace detail {

inline const char* skip_ws(const char* p, const char* end) {
    while (p < end) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p;
        else break;
    }
    return p;
}

// Extract a raw substring for the JSON value at *p (object, array, string,
// number, bool, null). Returns a pointer just past the value.
inline const char* extract_value(const char* p, const char* end, std::string& out) {
    p = skip_ws(p, end);
    if (p >= end) return p;
    const char* start = p;
    char c = *p;
    if (c == '"') {
        ++p;
        while (p < end) {
            if (*p == '\\' && p + 1 < end) { p += 2; continue; }
            if (*p == '"') { ++p; break; }
            ++p;
        }
    } else if (c == '{' || c == '[') {
        char open  = c;
        char close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (p < end) {
            if (*p == '"') {
                ++p;
                while (p < end) {
                    if (*p == '\\' && p + 1 < end) { p += 2; continue; }
                    if (*p == '"') { ++p; break; }
                    ++p;
                }
                continue;
            }
            if (*p == open)  { ++depth; ++p; continue; }
            if (*p == close) { --depth; ++p; if (depth == 0) break; continue; }
            ++p;
        }
    } else {
        while (p < end && *p != ',' && *p != '}' && *p != ']' &&
               *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            ++p;
        }
    }
    out.assign(start, p - start);
    return p;
}

inline bool strip_quotes(std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
        // Un-escape minimal set
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char n = s[i + 1];
                if      (n == '"')  out.push_back('"');
                else if (n == '\\') out.push_back('\\');
                else if (n == '/')  out.push_back('/');
                else if (n == 'n')  out.push_back('\n');
                else if (n == 'r')  out.push_back('\r');
                else if (n == 't')  out.push_back('\t');
                else                out.push_back(n);
                ++i;
            } else {
                out.push_back(s[i]);
            }
        }
        s = std::move(out);
        return true;
    }
    return false;
}

inline bool find_key(const char* p, const char* end, std::string_view key,
                     std::string& value_out, const char*& after_out) {
    // Expect we're at '{' or just past it. Advance to interior.
    p = skip_ws(p, end);
    if (p < end && *p == '{') ++p;
    while (p < end) {
        p = skip_ws(p, end);
        if (p < end && *p == '}') return false;
        if (p >= end || *p != '"') return false;
        std::string k;
        p = extract_value(p, end, k);
        strip_quotes(k);
        p = skip_ws(p, end);
        if (p >= end || *p != ':') return false;
        ++p;
        std::string v;
        p = extract_value(p, end, v);
        if (k == key) {
            value_out = std::move(v);
            after_out = p;
            return true;
        }
        p = skip_ws(p, end);
        if (p < end && *p == ',') ++p;
    }
    return false;
}

} // namespace detail

inline std::optional<ParsedCmd> parse_cmd(std::string_view json) {
    const char* p   = json.data();
    const char* end = p + json.size();

    // type must be "cmd"
    std::string type_val;
    const char* after;
    if (!detail::find_key(p, end, "type", type_val, after)) return std::nullopt;
    detail::strip_quotes(type_val);
    if (type_val != "cmd") return std::nullopt;

    ParsedCmd out;

    std::string id_val;
    if (!detail::find_key(p, end, "id", id_val, after)) return std::nullopt;
    try { out.id = std::stoll(id_val); } catch (...) { return std::nullopt; }

    std::string name_val;
    if (!detail::find_key(p, end, "name", name_val, after)) return std::nullopt;
    detail::strip_quotes(name_val);
    out.name = std::move(name_val);

    std::string args_val;
    if (detail::find_key(p, end, "args", args_val, after)) {
        out.args_json = std::move(args_val);
    } else {
        out.args_json = "{}";
    }

    return out;
}

// Extract a single string field from a small JSON object. Used by command
// handlers to pluck args like {"path":"..."} without dragging in a parser.
inline std::optional<std::string> get_string_field(std::string_view json, std::string_view key) {
    std::string v;
    const char* after;
    if (!detail::find_key(json.data(), json.data() + json.size(), key, v, after)) {
        return std::nullopt;
    }
    if (!detail::strip_quotes(v)) return std::nullopt;
    return v;
}

inline std::optional<double> get_number_field(std::string_view json, std::string_view key) {
    std::string v;
    const char* after;
    if (!detail::find_key(json.data(), json.data() + json.size(), key, v, after)) {
        return std::nullopt;
    }
    try { return std::stod(v); } catch (...) { return std::nullopt; }
}

} // namespace xi::proto
