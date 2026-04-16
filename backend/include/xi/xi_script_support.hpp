#pragma once
//
// xi_script_support.hpp — default implementations of the script export
// thunks described in xi_script.hpp.
//
// The backend's compile driver force-includes this header after the user
// source file (via /FI on MSVC, -include on gcc/clang), so the user never
// needs to know about thunks. Defining XI_SCRIPT_NO_DEFAULT_THUNKS before
// inclusion lets advanced users override the defaults.
//

#include "xi.hpp"
#include "xi_image.hpp"
#include "xi_script.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#ifndef XI_SCRIPT_NO_DEFAULT_THUNKS

namespace xi_script_detail {

// Trivial JSON string escape for names and string values. Same rules as
// the backend's xi_protocol.hpp but inlined here to keep this header
// independent of that file.
inline void esc(std::string& out, const char* s) {
    out.push_back('"');
    for (; *s; ++s) {
        char c = *s;
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)c);
                    out += b;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

// Cached map of gid → Image for xi_script_dump_image lookups within one
// run. Populated during xi_script_snapshot_vars and cleared by
// xi_script_reset.
struct ImageCacheEntry {
    uint32_t  gid;
    xi::Image img;
};

inline std::vector<ImageCacheEntry>& image_cache() {
    static std::vector<ImageCacheEntry> v;
    return v;
}

} // namespace xi_script_detail

XI_SCRIPT_EXPORT int xi_script_snapshot_vars(char* buf, int buflen) {
    using namespace xi_script_detail;
    auto snap = xi::ValueStore::current().snapshot();

    image_cache().clear();

    std::string out = "[";
    uint32_t next_gid = 100;
    for (size_t i = 0; i < snap.size(); ++i) {
        auto& e = snap[i];
        if (i) out += ",";
        out += "{\"name\":";
        esc(out, e.name.c_str());
        switch (e.kind) {
            case xi::VarKind::Number:
                out += ",\"kind\":\"number\",\"value\":";
                out += e.inline_json.empty() ? "null" : e.inline_json;
                break;
            case xi::VarKind::Boolean:
                out += ",\"kind\":\"boolean\",\"value\":";
                out += (e.inline_json == "true") ? "true" : "false";
                break;
            case xi::VarKind::String: {
                out += ",\"kind\":\"string\",\"value\":";
                std::string s;
                try { s = std::any_cast<std::string>(e.payload); } catch (...) {}
                esc(out, s.c_str());
                break;
            }
            case xi::VarKind::Image: {
                uint32_t gid = next_gid++;
                out += ",\"kind\":\"image\",\"gid\":";
                out += std::to_string(gid);
                out += ",\"raw\":false";
                try {
                    image_cache().push_back({gid, std::any_cast<xi::Image>(e.payload)});
                } catch (...) {}
                break;
            }
            case xi::VarKind::Json:
                out += ",\"kind\":\"json\",\"value\":";
                out += e.inline_json.empty() ? "null" : e.inline_json;
                break;
            default:
                out += ",\"kind\":\"custom\",\"value\":null";
        }
        out += "}";
    }
    out += "]";

    int needed = (int)out.size();
    if (buflen < needed + 1) return -needed;
    std::memcpy(buf, out.data(), out.size());
    buf[out.size()] = 0;
    return needed;
}

XI_SCRIPT_EXPORT int xi_script_dump_image(uint32_t gid, uint8_t* out, int cap,
                                          int* w, int* h, int* c) {
    for (auto& e : xi_script_detail::image_cache()) {
        if (e.gid != gid) continue;
        if (!e.img.data() || e.img.empty()) return 0;
        int needed = (int)e.img.size();
        if (cap < needed) return -needed;
        std::memcpy(out, e.img.data(), e.img.size());
        if (w) *w = e.img.width;
        if (h) *h = e.img.height;
        if (c) *c = e.img.channels;
        return needed;
    }
    return 0;
}

XI_SCRIPT_EXPORT int xi_script_list_params(char* buf, int buflen) {
    auto list = xi::ParamRegistry::instance().list();
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) out += ",";
        out += list[i]->as_json();
    }
    out += "]";
    int needed = (int)out.size();
    if (buflen < needed + 1) return -needed;
    std::memcpy(buf, out.data(), out.size());
    buf[out.size()] = 0;
    return needed;
}

XI_SCRIPT_EXPORT int xi_script_set_param(const char* name, const char* value_json) {
    auto* p = xi::ParamRegistry::instance().find(name);
    if (!p) return -1;
    return p->set_from_json(value_json) ? 0 : -2;
}

XI_SCRIPT_EXPORT void xi_script_reset() {
    xi::ValueStore::current().clear();
    xi_script_detail::image_cache().clear();
}

#endif // XI_SCRIPT_NO_DEFAULT_THUNKS
