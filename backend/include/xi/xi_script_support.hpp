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
#include <unordered_map>

// OpenMP thread cap (opt-in via project.json "openmp_max_threads"). When the
// backend compiles with /openmp for a positive cap it also passes
// /D XI_OMP_MAX_THREADS=<n>; we apply it once at DLL load so the script's
// parallel regions honour the project's ceiling — the author never has to call
// omp_set_num_threads(). Guarded by _OPENMP so non-OpenMP builds never touch
// <omp.h>. (A cap of -1 / "all cores" compiles with /openmp but no cap define,
// so this is a no-op and the OpenMP default of all cores applies.)
#ifdef _OPENMP
#include <omp.h>
namespace xi_script_detail {
inline int apply_omp_thread_cap_() {
#ifdef XI_OMP_MAX_THREADS
    if ((XI_OMP_MAX_THREADS) > 0) omp_set_num_threads((XI_OMP_MAX_THREADS));
#endif
    return 0;
}
inline int g_omp_cap_applied_ = apply_omp_thread_cap_();  // runs at DLL load
} // namespace xi_script_detail
#endif

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

} // namespace xi_script_detail

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
    // VAR value-store + image cache were removed; nothing per-run to reset here.
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

// Symmetric read of set_instance_def: serialize an instance's full def (whatever
// get_def() returns, incl. any assets the plugin round-trips like image_png_b64).
// Same buffer protocol as xi_script_exchange_instance: returns bytes written, or
// -needed when the buffer is too small so the caller grows + retries. -1 if the
// instance isn't found in the script's registry.
XI_SCRIPT_EXPORT int xi_script_get_instance_def(const char* name, char* buf, int buflen) {
    auto inst = xi::InstanceRegistry::instance().find(name);
    if (!inst) return -1;
    std::string def = inst->get_def();
    if (def.empty()) def = "{}";
    int needed = (int)def.size();
    if (buflen < needed + 1) return -needed;
    std::memcpy(buf, def.data(), def.size());
    buf[def.size()] = 0;
    return needed;
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
static void* g_trigger_leader_fn_   = nullptr;
static void* g_trigger_meta_fn_     = nullptr;   // ABI v5: emit_trigger_record metadata doc

// Status callback. Host sets this so xi::status(text) publishes the script's
// latest status string to the host status registry. Signature: void(const char*).
static void* g_status_fn_           = nullptr;

// Result callback. Host sets this so xi::result(code,msg) records the one per-run
// verdict. Signature: void(int code, const char* msg).
static void* g_result_fn_           = nullptr;

// Per-run context. Set by the host before each xi_inspect_entry call,
// cleared after. Currently just the optional `frame_path` arg from
// cmd:run; future per-run fields (run_id, request id, etc.) join here.
//
// Scripts read these via accessors in xi_io.hpp — never touch the raw
// globals. Host writes them via xi_script_set_run_context.
//
// `g_run_frame_path_` is sized to 1024 — paths longer than that are
// truncated. Plenty for any reasonable file system.
//
// thread_local so multiple dispatcher threads (project.json
// `parallelism.dispatch_threads > 1`) each see their own per-run
// path. Each thread's `xi_script_set_run_context` writes its own
// slot; `xi::current_frame_path()` reads it back from the same
// thread.
static thread_local char g_run_frame_path_[1024] = {0};

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

// Separate symbol so adding the leader callback doesn't break the older
// 3-arg signature scripts already export. Hosts that load this DLL look up
// the symbol via GetProcAddress; missing = leader unavailable, in which
// case xi::Trigger::primary_source() falls back to sources().front().
XI_SCRIPT_EXPORT void xi_script_set_trigger_leader_callback(void* leader_fn) {
    g_trigger_leader_fn_ = leader_fn;
}

// ABI v5: metadata-doc callback (emit_trigger_record). Separate symbol for the
// same back-compat reason as the leader callback; missing ⇒ meta unavailable
// and xi::Trigger::meta() returns an empty Record.
XI_SCRIPT_EXPORT void xi_script_set_trigger_meta_callback(void* meta_fn) {
    g_trigger_meta_fn_ = meta_fn;
}

// Optional: install a status callback for xi::status(text). Scripts that don't
// include xi_status.hpp leave this null.
XI_SCRIPT_EXPORT void xi_script_set_status_callback(void* fn) {
    g_status_fn_ = fn;
}

// Optional: install a result callback for xi::result(code,msg). Scripts that
// don't include xi_result.hpp leave this null.
XI_SCRIPT_EXPORT void xi_script_set_result_callback(void* fn) {
    g_result_fn_ = fn;
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
    // Replace the state Record by parsing the JSON (yyjson via from_json_bytes).
    xi::state() = xi::Record::from_json_bytes((const uint8_t*)json, std::strlen(json));
    return 0;
}

// State schema version — bump when the shape of xi::state() changes
// in a way the previous DLL's persisted JSON would default-fill
// incorrectly. Backend records the version alongside saved state and
// drops the state on mismatch instead of silently restoring garbage.
//
// Default 0 means "unversioned" — backend skips the migration check
// and restores blindly (legacy back-compat). Register from user code:
//
//   XI_STATE_SCHEMA(2);   // file-scope macro
//
// or manually:
//
//   static int _sv = (xi::set_state_schema_version(2), 0);
//
// The export below reads the runtime atomic, so the user's static
// initialiser (which runs at DLL load, BEFORE the host's first
// xi_script_set_state) wins.
XI_SCRIPT_EXPORT int xi_script_state_schema_version(void) {
    return xi::state_schema_version();
}

#endif // XI_SCRIPT_NO_DEFAULT_THUNKS
