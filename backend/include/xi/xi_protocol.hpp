#pragma once
//
// xi_protocol.hpp — xInsp2 WebSocket protocol types (C++ side).
//
// Canonical schema lives in docs/reference/ws-protocol.md (with the JSON shapes
// in contract/schemas/). This header mirrors it as plain C++ structs plus minimal
// JSON encode/decode helpers.
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
#include <exception>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "xi_json_escape.hpp"

namespace xi::proto {

// NOTE: the `vars` wire enum (VarKindWire) + VarItem/Vars structs were removed
// with the v9 vars/value-store teardown — nothing emits a `vars` frame anymore.
// Script output now leaves as the `expose` plugin's self-framed XEX1 binary
// frame (plugins/expose/src/expose.cpp), not a core protocol type.

// NOTE: the old binary "preview header" (gid/codec/w/h/ch, 20-byte big-endian)
// and its Codec enum were removed with the v9 vars/preview-core teardown. The
// `expose` plugin now frames its own output as a self-describing XEX1 binary
// frame (magic + msgpack; see plugins/expose/src/expose.cpp); the core is a dumb
// byte pipe for it (host emit_binary → broadcast), so no core-side header type.

// ---------- JSON string escape / unescape ----------

// The escape primitive moved to xi_json_escape.hpp (one copy for the whole
// backend). These keep the xi::proto:: names the protocol writers use.
inline void json_escape_into(std::string& out, std::string_view s) {
    ::xi::json_escape_into(out, s);
}

inline std::string json_escape(std::string_view s) {
    return ::xi::json_escape(s);
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
        if (!data_json.empty()) {
            out += ",\"data\":";
            // TRUST BOUNDARY: data_json is appended verbatim. Every caller
            // that populates this field is responsible for emitting well-
            // formed JSON — we do NOT revalidate here. Handler code inside
            // the backend is the trust origin; cJSON-backed builders
            // (xi::Json, xi::Record::data_json) are always safe, as are
            // string-concat sites that use json_escape_into on every
            // dynamic value. Ad-hoc `"{\"foo\":\"" + bar + "\"}"` is the
            // pattern to watch — must escape `bar`.
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
        // Un-escape. The matching writers (json_escape_into / pm_json_escape /
        // the Record + script-support escapers) emit `\b` `\f` and `\uXXXX` for
        // control chars — decoding only the \"/\\/\n/\r/\t subset silently
        // corrupted any name/path carrying a control char or non-ASCII via \u
        // (e.g. `` came back as the literal "u0007").
        std::string out;
        out.reserve(s.size());
        auto emit_utf8 = [&out](unsigned cp) {
            if (cp <= 0x7F) out.push_back((char)cp);
            else if (cp <= 0x7FF) {
                out.push_back((char)(0xC0 | (cp >> 6)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            } else if (cp <= 0xFFFF) {
                out.push_back((char)(0xE0 | (cp >> 12)));
                out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            } else {
                out.push_back((char)(0xF0 | (cp >> 18)));
                out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            }
        };
        auto hex4 = [&s](size_t pos, unsigned& v) -> bool {
            if (pos + 4 > s.size()) return false;
            v = 0;
            for (int k = 0; k < 4; ++k) {
                char c = s[pos + k]; v <<= 4;
                if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
                else return false;
            }
            return true;
        };
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char n = s[i + 1];
                if      (n == '"')  { out.push_back('"');  ++i; }
                else if (n == '\\') { out.push_back('\\'); ++i; }
                else if (n == '/')  { out.push_back('/');  ++i; }
                else if (n == 'n')  { out.push_back('\n'); ++i; }
                else if (n == 'r')  { out.push_back('\r'); ++i; }
                else if (n == 't')  { out.push_back('\t'); ++i; }
                else if (n == 'b')  { out.push_back('\b'); ++i; }
                else if (n == 'f')  { out.push_back('\f'); ++i; }
                else if (n == 'u') {
                    unsigned cp;
                    if (hex4(i + 2, cp)) {
                        size_t adv = i + 5;   // index of the last hex digit
                        // Combine a UTF-16 surrogate pair (\uD800-\uDBFF + low).
                        if (cp >= 0xD800 && cp <= 0xDBFF &&
                            adv + 2 < s.size() && s[adv + 1] == '\\' && s[adv + 2] == 'u') {
                            unsigned lo;
                            if (hex4(adv + 3, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                adv += 6;
                            }
                        }
                        emit_utf8(cp);
                        i = adv;              // loop ++i steps past the escape
                    } else { out.push_back('u'); ++i; }   // malformed — best effort
                }
                else { out.push_back(n); ++i; }
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

// Best-effort recovery of a command's numeric `id` from an envelope that
// parse_cmd() REJECTED (wrong/missing `type`, missing `name`, etc.). Returns the
// id only when one is present and parseable, so an otherwise-malformed command
// can still get a CORRELATED error reply instead of stalling the client to its
// timeout (review 09 finding 2). Returns nullopt when there is genuinely nothing
// to correlate to (no id, or an id that overflows int64 — e.g. a JS bigint), in
// which case the caller falls back to a log-only reject.
inline std::optional<int64_t> recover_cmd_id(std::string_view json) {
    std::string v;
    const char* after;
    if (!detail::find_key(json.data(), json.data() + json.size(), "id", v, after))
        return std::nullopt;
    detail::strip_quotes(v);   // tolerate a quoted "id":"7" as well as bare 7
    try {
        size_t pos = 0;
        long long r = std::stoll(v, &pos);
        return r;
    } catch (...) {
        return std::nullopt;
    }
}

// The single dispatch shell (adoption-map item 1 / review 09 findings 1-2).
// Runs the whole command lifecycle behind ONE top-level guard so that:
//   * any std::exception (or unexpected throw) escaping a handler becomes a
//     structured `rsp` ok:false correlated to the command id — the contract
//     docs/reference/ws-protocol.md already promises — instead of unwinding out
//     of the serve loop into std::terminate (whole-backend death);
//   * a malformed envelope replies with a correlated error when an id is
//     recoverable, else logs; either way it counts a reject.
//
// Parameterised on its side effects (not on ws::Server) so the guard itself is
// unit-testable with fakes; the production wiring in service_main.cpp is the
// only real instantiation. Contracts:
//   send_err(int64_t id, std::string msg)   — emit rsp ok:false with `msg`.
//   send_log(std::string msg)               — emit a log-only line (no id to
//                                             correlate to).
//   on_reject()                             — bump the visible malformed-cmd
//                                             reject counter.
//   invoke(name, id, parsed) -> bool        — look up & call a handler; false
//                                             means "no such command". MUST let
//                                             a handler throw propagate here.
template <class SendErr, class SendLog, class OnReject, class Invoke>
inline void dispatch_command_guarded(std::string_view text,
                                     SendErr&&  send_err,
                                     SendLog&&  send_log,
                                     OnReject&& on_reject,
                                     Invoke&&   invoke) {
    auto parsed = parse_cmd(text);
    if (!parsed) {
        on_reject();
        if (auto id = recover_cmd_id(text)) {
            send_err(*id, std::string("malformed command"));
        } else {
            send_log(std::string("malformed cmd: ") +
                     std::string(text.substr(0, 128)));
        }
        return;
    }
    const int64_t id = parsed->id;
    try {
        if (!invoke(std::string_view(parsed->name), id, &*parsed))
            send_err(id, std::string("unknown command: ") + parsed->name);
    } catch (const std::exception& e) {
        send_err(id, std::string(e.what()));
    } catch (...) {
        send_err(id, std::string("unknown error in command handler"));
    }
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
