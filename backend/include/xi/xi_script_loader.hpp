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

#include <cstdint>
#include <string>

namespace xi::script {

struct LoadedScript {
    HMODULE handle = nullptr;
    std::string path;

    using InspectFn          = void (*)(int frame);
    using SnapshotFn         = int  (*)(char* buf, int buflen);
    using DumpImageFn        = int  (*)(uint32_t gid, uint8_t* out, int cap, int* w, int* h, int* c);
    using ListParamsFn       = int  (*)(char* buf, int buflen);
    using SetParamFn         = int  (*)(const char* name, const char* value_json);
    using ResetFn            = void (*)();
    using ListInstancesFn    = int  (*)(char* buf, int buflen);
    using SetInstanceDefFn   = int  (*)(const char* name, const char* def_json);
    using ExchangeInstanceFn = int  (*)(const char* name, const char* cmd_json, char* rsp, int rsplen);
    using GetStateFn         = int  (*)(char* buf, int buflen);
    using SetStateFn         = int  (*)(const char* json);
    using SetUseCallbacksFn  = void (*)(void* process_fn, void* exchange_fn,
                                        void* grab_fn, void* host_api);

    InspectFn          inspect          = nullptr;
    SnapshotFn         snapshot         = nullptr;
    DumpImageFn        dump_image       = nullptr;
    ListParamsFn       list_params      = nullptr;
    SetParamFn         set_param        = nullptr;
    ResetFn            reset            = nullptr;
    ListInstancesFn    list_instances   = nullptr;
    SetInstanceDefFn   set_instance_def = nullptr;
    ExchangeInstanceFn exchange_instance = nullptr;
    GetStateFn         get_state         = nullptr;
    SetStateFn         set_state         = nullptr;
    SetUseCallbacksFn  set_use_callbacks = nullptr;

    bool ok() const { return handle && inspect; }
};

inline bool load_script(const std::string& dll_path, LoadedScript& out, std::string& err) {
    HMODULE h = LoadLibraryA(dll_path.c_str());
    if (!h) {
        DWORD e = GetLastError();
        err = "LoadLibrary failed (" + std::to_string(e) + ") for " + dll_path;
        return false;
    }
    out.handle = h;
    out.path   = dll_path;
    out.inspect     = reinterpret_cast<LoadedScript::InspectFn>(GetProcAddress(h, "xi_inspect_entry"));
    out.snapshot    = reinterpret_cast<LoadedScript::SnapshotFn>(GetProcAddress(h, "xi_script_snapshot_vars"));
    out.dump_image  = reinterpret_cast<LoadedScript::DumpImageFn>(GetProcAddress(h, "xi_script_dump_image"));
    out.list_params = reinterpret_cast<LoadedScript::ListParamsFn>(GetProcAddress(h, "xi_script_list_params"));
    out.set_param   = reinterpret_cast<LoadedScript::SetParamFn>(GetProcAddress(h, "xi_script_set_param"));
    out.reset            = reinterpret_cast<LoadedScript::ResetFn>(GetProcAddress(h, "xi_script_reset"));
    out.list_instances   = reinterpret_cast<LoadedScript::ListInstancesFn>(GetProcAddress(h, "xi_script_list_instances"));
    out.set_instance_def = reinterpret_cast<LoadedScript::SetInstanceDefFn>(GetProcAddress(h, "xi_script_set_instance_def"));
    out.exchange_instance = reinterpret_cast<LoadedScript::ExchangeInstanceFn>(GetProcAddress(h, "xi_script_exchange_instance"));
    out.get_state         = reinterpret_cast<LoadedScript::GetStateFn>(GetProcAddress(h, "xi_script_get_state"));
    out.set_state         = reinterpret_cast<LoadedScript::SetStateFn>(GetProcAddress(h, "xi_script_set_state"));
    out.set_use_callbacks = reinterpret_cast<LoadedScript::SetUseCallbacksFn>(GetProcAddress(h, "xi_script_set_use_callbacks"));
    if (!out.inspect) {
        err = "script missing xi_inspect_entry export";
        FreeLibrary(h);
        out.handle = nullptr;
        return false;
    }
    return true;
}

inline void unload_script(LoadedScript& s) {
    if (s.handle) {
        FreeLibrary(s.handle);
    }
    s = LoadedScript{};
}

} // namespace xi::script
