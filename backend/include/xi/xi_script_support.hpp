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
#include "xi_record.hpp"
#include "xi_script.hpp"
#include "xi_state.hpp"

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
            case xi::VarKind::Json: {
                // Check if payload is a Record (has images)
                bool is_record = false;
                try {
                    auto& rec = std::any_cast<const xi::Record&>(e.payload);
                    is_record = true;
                    out += ",\"kind\":\"record\",\"data\":";
                    out += rec.data_json();
                    out += ",\"image_keys\":";
                    out += rec.image_keys_json();
                    // Assign gids for each image in the record
                    out += ",\"images\":{";
                    bool img_first = true;
                    for (auto& [ik, iv] : rec.images()) {
                        if (!img_first) out += ",";
                        img_first = false;
                        uint32_t gid = next_gid++;
                        esc(out, ik.c_str());
                        out += ":";
                        out += std::to_string(gid);
                        image_cache().push_back({gid, iv});
                    }
                    out += "}";
                } catch (...) {}
                if (!is_record) {
                    out += ",\"kind\":\"json\",\"value\":";
                    out += e.inline_json.empty() ? "null" : e.inline_json;
                }
                break;
            }
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

// --- Instance registry thunks ---

XI_SCRIPT_EXPORT int xi_script_list_instances(char* buf, int buflen) {
    auto list = xi::InstanceRegistry::instance().list();
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) out += ",";
        out += "{\"name\":";
        xi_script_detail::esc(out, list[i]->name().c_str());
        out += ",\"plugin\":";
        xi_script_detail::esc(out, list[i]->plugin_name().c_str());
        out += ",\"def\":";
        std::string def = list[i]->get_def();
        out += def.empty() ? "{}" : def;
        out += "}";
    }
    out += "]";
    int needed = (int)out.size();
    if (buflen < needed + 1) return -needed;
    std::memcpy(buf, out.data(), out.size());
    buf[out.size()] = 0;
    return needed;
}

XI_SCRIPT_EXPORT int xi_script_set_instance_def(const char* name, const char* def_json) {
    auto inst = xi::InstanceRegistry::instance().find(name);
    if (!inst) return -1;
    return inst->set_def(def_json) ? 0 : -2;
}

XI_SCRIPT_EXPORT int xi_script_exchange_instance(const char* name, const char* cmd_json,
                                                  char* rsp_buf, int rsp_buflen) {
    auto inst = xi::InstanceRegistry::instance().find(name);
    if (!inst) return -1;
    std::string rsp = inst->exchange(cmd_json);
    int needed = (int)rsp.size();
    if (rsp_buflen < needed + 1) return -needed;
    std::memcpy(rsp_buf, rsp.data(), rsp.size());
    rsp_buf[rsp.size()] = 0;
    return needed;
}

// --- xi::use() callback storage ---
//
// Stored as void* to avoid xi_abi.h dependency. xi_use.hpp casts them.
//
// Lifetime invariant: these globals live inside the USER SCRIPT DLL
// (force-included via xi_script_support.hpp). They are written from
// the host side via xi_script_set_use_callbacks once per DLL load,
// and consumed from script code until DLL unload. The host MUST NOT
// retain any pointer into these after calling FreeLibrary — all
// reads happen inside the script's address space only.
static void* g_use_process_fn_   = nullptr;
static void* g_use_exchange_fn_  = nullptr;
static void* g_use_grab_fn_      = nullptr;
// Pointer to backend's xi_host_api — image_create / image_data /
// image_release etc. all operate on the BACKEND's ImagePool, which is the
// only pool plugins see via their own host_api. Without this, the script's
// per-DLL ImagePool singleton would create handles invisible to plugins.
static void* g_use_host_api_     = nullptr;
// Trigger access callbacks — set whenever continuous mode is bus-driven.
// Signatures (cast in xi_use.hpp):
//   trigger_info_fn  : void(xi_current_trigger_info* out)
//   trigger_image_fn : xi_image_handle(const char* source)
//   trigger_sources_fn : int32_t(char* buf, int32_t buflen)  // \n-separated
static void* g_trigger_info_fn_     = nullptr;
static void* g_trigger_image_fn_    = nullptr;
static void* g_trigger_sources_fn_  = nullptr;

// Breakpoint callback (S3). Host sets this so xi::breakpoint(label)
// inside user script blocks until the WS client sends `cmd: resume`.
// Signature: void(const char* label).
static void* g_breakpoint_fn_       = nullptr;

// Per-run context. Set by the host before each xi_inspect_entry call,
// cleared after. Currently just the optional `frame_path` arg from
// cmd:run; future per-run fields (run_id, request id, etc.) join here.
//
// Scripts read these via accessors in xi_io.hpp — never touch the raw
// globals. Host writes them via xi_script_set_run_context.
//
// `g_run_frame_path_` is sized to 1024 — paths longer than that are
// truncated. Plenty for any reasonable file system.
static char g_run_frame_path_[1024] = {0};

XI_SCRIPT_EXPORT void xi_script_set_use_callbacks(
    void* process_fn, void* exchange_fn, void* grab_fn,
    void* host_api)
{
    g_use_process_fn_   = process_fn;
    g_use_exchange_fn_  = exchange_fn;
    g_use_grab_fn_      = grab_fn;
    g_use_host_api_     = host_api;
}

// Optional follow-up call (newer hosts only). Older scripts that don't
// know about triggers simply leave these null and current_trigger() is
// inactive — back-compat preserved.
XI_SCRIPT_EXPORT void xi_script_set_trigger_callbacks(
    void* info_fn, void* image_fn, void* sources_fn)
{
    g_trigger_info_fn_    = info_fn;
    g_trigger_image_fn_   = image_fn;
    g_trigger_sources_fn_ = sources_fn;
}

// Optional: install a breakpoint callback for xi::breakpoint(label).
// Scripts that don't include xi_breakpoint.hpp leave this null.
XI_SCRIPT_EXPORT void xi_script_set_breakpoint_callback(void* fn) {
    g_breakpoint_fn_ = fn;
}

// Per-run context setter (called by host before each xi_inspect_entry,
// optional cleanup after). `frame_path` may be null/empty when the
// caller didn't provide one — scripts get back an empty string from
// xi::current_frame_path() in that case.
XI_SCRIPT_EXPORT void xi_script_set_run_context(const char* frame_path) {
    if (!frame_path) frame_path = "";
    size_t n = 0;
    while (frame_path[n] && n + 1 < sizeof(g_run_frame_path_)) {
        g_run_frame_path_[n] = frame_path[n];
        ++n;
    }
    g_run_frame_path_[n] = 0;
}

// Watchdog cancel flag setter — host sets this when inspect overruns
// its deadline; script's `xi::cancellation_requested()` returns true
// while it's set. Long-running ops poll this and exit early. Host
// clears it after the inspect returns (or after watchdog falls back
// to TerminateThread).
XI_SCRIPT_EXPORT void xi_script_set_global_cancel(int set) {
    xi::global_cancel_flag().store(set != 0, std::memory_order_relaxed);
}

// --- Persistent state thunks ---

XI_SCRIPT_EXPORT int xi_script_get_state(char* buf, int buflen) {
    std::string json = xi::state().data_json();
    int needed = (int)json.size();
    if (buflen < needed + 1) return -needed;
    std::memcpy(buf, json.data(), json.size());
    buf[json.size()] = 0;
    return needed;
}

XI_SCRIPT_EXPORT int xi_script_set_state(const char* json) {
    if (!json) return -1;
    cJSON* parsed = cJSON_Parse(json);
    if (!parsed) return -1;
    // Replace the state Record's internal JSON tree
    xi::Record& s = xi::state();
    // Clear and rebuild
    s = xi::Record();
    cJSON* item = parsed->child;
    while (item) {
        if (cJSON_IsNumber(item))      s.set(item->string, item->valuedouble);
        else if (cJSON_IsBool(item))   s.set(item->string, cJSON_IsTrue(item) ? true : false);
        else if (cJSON_IsString(item)) s.set(item->string, std::string(item->valuestring));
        else                           s.set_raw(item->string, cJSON_Duplicate(item, true));
        item = item->next;
    }
    cJSON_Delete(parsed);
    return 0;
}

#endif // XI_SCRIPT_NO_DEFAULT_THUNKS
