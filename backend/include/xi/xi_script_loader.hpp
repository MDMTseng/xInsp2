#pragma once
//
// xi_script_loader.hpp — LoadLibrary the compiled user script and hold
// resolved function pointers for the entry and thunks.
//

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#include "xi_image_pool.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace xi::script {

struct LoadedScript {
    HMODULE handle = nullptr;
    std::string path;
    // Owner ledger id for the ImagePool. Allocated on each successful
    // load_script; release_all_for(owner) is called on unload to sweep
    // any handles the script forgot. Anonymous (0) until load completes.
    ImagePoolOwnerId owner_id = 0;

    // Keeps the loaded module mapped while ANY copy of this LoadedScript is
    // alive. run_one_inspection snapshots g_script BY VALUE and runs inspect()
    // OUTSIDE g_script_mu; a concurrent compile_and_load unloads+reloads
    // g_script under that lock and must NOT FreeLibrary the module out from
    // under the in-flight (possibly detached cmd:run / one-shot) call. The
    // deleter performs the ImagePool owner-sweep + FreeLibrary only when the
    // LAST copy drops — i.e. once every in-flight inspect that snapshotted this
    // script has returned. The raw fn pointers below stay valid as long as a
    // copy holds this. (Held as shared_ptr<void> so this header needn't pull
    // the HMODULE type into the deleter's signature.)
    std::shared_ptr<void> module_lifetime;

    using InspectFn          = void (*)(int frame);
    using SnapshotFn         = int  (*)(char* buf, int buflen);
    using DumpImageFn        = int  (*)(uint32_t gid, uint8_t* out, int cap, int* w, int* h, int* c);
    using ListParamsFn       = int  (*)(char* buf, int buflen);
    using SetParamFn         = int  (*)(const char* name, const char* value_json);
    using ResetFn            = void (*)();
    using ListInstancesFn    = int  (*)(char* buf, int buflen);
    using SetInstanceDefFn   = int  (*)(const char* name, const char* def_json);
    using GetInstanceDefFn   = int  (*)(const char* name, char* buf, int buflen);
    using ExchangeInstanceFn = int  (*)(const char* name, const char* cmd_json, char* rsp, int rsplen);
    using GetStateFn         = int  (*)(char* buf, int buflen);
    using SetStateFn         = int  (*)(const char* json);
    using SetUseCallbacksFn  = void (*)(void* process_fn, void* exchange_fn,
                                        void* grab_fn, void* host_api);
    using SetTriggerCallbacksFn = void (*)(void* info_fn, void* image_fn,
                                           void* sources_fn);
    using SetRunContextFn         = void (*)(const char* frame_path);
    using SetGlobalCancelFn       = void (*)(int set);
    using StateSchemaVersionFn    = int  (*)(void);

    InspectFn          inspect          = nullptr;
    SnapshotFn         snapshot         = nullptr;
    DumpImageFn        dump_image       = nullptr;
    ListParamsFn       list_params      = nullptr;
    SetParamFn         set_param        = nullptr;
    ResetFn            reset            = nullptr;
    ListInstancesFn    list_instances   = nullptr;
    SetInstanceDefFn   set_instance_def = nullptr;
    GetInstanceDefFn   get_instance_def = nullptr;
    ExchangeInstanceFn exchange_instance = nullptr;
    GetStateFn         get_state         = nullptr;
    SetStateFn         set_state         = nullptr;
    SetUseCallbacksFn  set_use_callbacks = nullptr;
    SetTriggerCallbacksFn set_trigger_callbacks = nullptr;
    using SetTriggerLeaderCallbackFn = void (*)(void* leader_fn);
    SetTriggerLeaderCallbackFn set_trigger_leader_callback = nullptr;
    // ABI v5: metadata-doc callback (emit_trigger_record). Optional symbol.
    using SetTriggerMetaCallbackFn = void (*)(void* meta_fn);
    SetTriggerMetaCallbackFn set_trigger_meta_callback = nullptr;
    using SetStatusCallbackFn = void (*)(void* fn);
    SetStatusCallbackFn set_status_callback = nullptr;
    using SetResultCallbackFn = void (*)(void* fn);
    SetResultCallbackFn set_result_callback = nullptr;
    SetRunContextFn    set_run_context  = nullptr;
    SetGlobalCancelFn  set_global_cancel = nullptr;
    StateSchemaVersionFn state_schema_version = nullptr;

    bool ok() const { return handle && inspect; }
};

inline bool load_script(const std::string& dll_path, LoadedScript& out, std::string& err) {
    // LOAD_LIBRARY_SEARCH_DEFAULT_DIRS keeps the app dir (OpenCV/turbojpeg/IPP are
    // deployed beside xinsp-backend.exe) + System32; LOAD_LIBRARY_SEARCH_USER_DIRS
    // honours the project folder the host added via AddDllDirectory, so a script's
    // statically-linked external dependency DLL can live in the project folder.
    // (CWD/PATH are dropped — same trade as the plugin loader; deps belong in the
    // app dir or the project folder, not on a wandering PATH.)
    // TODO(linux): dlopen + RPATH/$ORIGIN; no LOAD_LIBRARY_SEARCH analogue.
    HMODULE h = LoadLibraryExA(dll_path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                               LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!h) {
        DWORD e = GetLastError();
        err = "LoadLibrary failed (" + std::to_string(e) + ") for " + dll_path;
        return false;
    }
    out.handle = h;
    out.path   = dll_path;
    // Contract: every pointer below is OPTIONAL except `inspect`. Missing
    // symbols stay null; callers MUST null-check before invoking. The
    // snapshot / reset / param / instance / state / use / trigger thunks
    // are all auto-generated by xi_script_support.hpp — only present if
    // the user's script actually uses the corresponding header. Only
    // `xi_inspect_entry` (line below) triggers load failure if absent.
    out.inspect     = reinterpret_cast<LoadedScript::InspectFn>(GetProcAddress(h, "xi_inspect_entry"));
    out.snapshot    = reinterpret_cast<LoadedScript::SnapshotFn>(GetProcAddress(h, "xi_script_snapshot_vars"));
    out.dump_image  = reinterpret_cast<LoadedScript::DumpImageFn>(GetProcAddress(h, "xi_script_dump_image"));
    out.list_params = reinterpret_cast<LoadedScript::ListParamsFn>(GetProcAddress(h, "xi_script_list_params"));
    out.set_param   = reinterpret_cast<LoadedScript::SetParamFn>(GetProcAddress(h, "xi_script_set_param"));
    out.reset            = reinterpret_cast<LoadedScript::ResetFn>(GetProcAddress(h, "xi_script_reset"));
    out.list_instances   = reinterpret_cast<LoadedScript::ListInstancesFn>(GetProcAddress(h, "xi_script_list_instances"));
    out.set_instance_def = reinterpret_cast<LoadedScript::SetInstanceDefFn>(GetProcAddress(h, "xi_script_set_instance_def"));
    out.get_instance_def = reinterpret_cast<LoadedScript::GetInstanceDefFn>(GetProcAddress(h, "xi_script_get_instance_def"));
    out.exchange_instance = reinterpret_cast<LoadedScript::ExchangeInstanceFn>(GetProcAddress(h, "xi_script_exchange_instance"));
    out.get_state         = reinterpret_cast<LoadedScript::GetStateFn>(GetProcAddress(h, "xi_script_get_state"));
    out.set_state         = reinterpret_cast<LoadedScript::SetStateFn>(GetProcAddress(h, "xi_script_set_state"));
    out.set_use_callbacks = reinterpret_cast<LoadedScript::SetUseCallbacksFn>(GetProcAddress(h, "xi_script_set_use_callbacks"));
    out.set_trigger_callbacks = reinterpret_cast<LoadedScript::SetTriggerCallbacksFn>(GetProcAddress(h, "xi_script_set_trigger_callbacks"));
    out.set_trigger_leader_callback = reinterpret_cast<LoadedScript::SetTriggerLeaderCallbackFn>(GetProcAddress(h, "xi_script_set_trigger_leader_callback"));
    out.set_trigger_meta_callback = reinterpret_cast<LoadedScript::SetTriggerMetaCallbackFn>(GetProcAddress(h, "xi_script_set_trigger_meta_callback"));
    out.set_status_callback = reinterpret_cast<LoadedScript::SetStatusCallbackFn>(GetProcAddress(h, "xi_script_set_status_callback"));
    out.set_result_callback = reinterpret_cast<LoadedScript::SetResultCallbackFn>(GetProcAddress(h, "xi_script_set_result_callback"));
    out.set_run_context = reinterpret_cast<LoadedScript::SetRunContextFn>(GetProcAddress(h, "xi_script_set_run_context"));
    out.set_global_cancel = reinterpret_cast<LoadedScript::SetGlobalCancelFn>(GetProcAddress(h, "xi_script_set_global_cancel"));
    out.state_schema_version = reinterpret_cast<LoadedScript::StateSchemaVersionFn>(GetProcAddress(h, "xi_script_state_schema_version"));
    if (!out.inspect) {
        err = "script missing xi_inspect_entry export";
        FreeLibrary(h);
        out.handle = nullptr;
        return false;
    }
    out.owner_id = ImagePool::alloc_owner_id();
    // Take ownership of the module via a refcounted lifetime token. Teardown
    // (owner-sweep + FreeLibrary) is deferred to the deleter so it runs only
    // after the last copy — including any in-flight detached inspect that
    // snapshotted this LoadedScript — has been released.
    ImagePoolOwnerId oid = out.owner_id;
    out.module_lifetime = std::shared_ptr<void>(h, [oid](void* m) {
        // A shared_ptr deleter must never throw, and may run late (during static
        // destruction, if the last copy is dropped then). Guard both: skip the
        // pool sweep once the ImagePool singleton is gone (touching the destroyed
        // Meyers singleton is UB), and swallow any exception.
        try {
            if (oid != 0 && g_image_pool_alive.load(std::memory_order_acquire)) {
                int swept = ImagePool::instance().release_all_for(oid);
                if (swept > 0) {
                    std::fprintf(stderr,
                        "[xinsp2] script unload: swept %d leaked image handle(s)\n",
                        swept);
                }
            }
            if (m) FreeLibrary(static_cast<HMODULE>(m));
        } catch (...) { /* deleter must not throw */ }
    });
    return true;
}

inline void unload_script(LoadedScript& s) {
    // Drop THIS slot's reference to the module. The owner-sweep + FreeLibrary
    // happen in module_lifetime's deleter when the LAST live copy is released
    // — deferring teardown past any in-flight detached run that snapshotted the
    // script by value (see LoadedScript::module_lifetime). In the common case
    // (no in-flight copy) this drops the last ref and tears down synchronously,
    // matching the previous behaviour.
    s = LoadedScript{};
}

} // namespace xi::script
