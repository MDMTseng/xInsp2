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

    using InspectFn      = void (*)(int frame);
    using SnapshotFn     = int  (*)(char* buf, int buflen);
    using DumpImageFn    = int  (*)(uint32_t gid, uint8_t* out, int cap, int* w, int* h, int* c);
    using ListParamsFn   = int  (*)(char* buf, int buflen);
    using SetParamFn     = int  (*)(const char* name, const char* value_json);
    using ResetFn        = void (*)();

    InspectFn    inspect    = nullptr;
    SnapshotFn   snapshot   = nullptr;
    DumpImageFn  dump_image = nullptr;
    ListParamsFn list_params = nullptr;
    SetParamFn   set_param   = nullptr;
    ResetFn      reset       = nullptr;

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
    out.reset       = reinterpret_cast<LoadedScript::ResetFn>(GetProcAddress(h, "xi_script_reset"));
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
