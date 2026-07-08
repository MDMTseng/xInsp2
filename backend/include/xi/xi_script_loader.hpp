#pragma once
//
// xi_script_loader.hpp — LoadLibrary the compiled user script and hold
// resolved function pointers for the entry and thunks.
//

#include "xi_dynlib.hpp"   // HMODULE/LoadLibrary/GetProcAddress/FreeLibrary (win32 or dlopen shim)

#include "xi_image_pool.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// A4: the explicit-trigger entry takes a pointer to this host-filled C-ABI view
// (full definition in xi_use.hpp). Forward-declared here so the loader needs
// only the pointer type for its function-pointer typedef.
struct xi_trigger_view;

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
    // A4: explicit-trigger entry. The host fills a xi_trigger_view and passes it
    // in, eliminating the ambient thread_local seam. Resolved in preference to
    // `inspect` when present; a script exports EITHER this or the legacy entry.
    using InspectTvFn        = void (*)(const ::xi_trigger_view* tv, int frame);
    using ListParamsFn       = int  (*)(char* buf, int buflen);
    using SetParamFn         = int  (*)(const char* name, const char* value_json);
    using ResetFn            = void (*)();
    using ListInstancesFn    = int  (*)(char* buf, int buflen);
    using SetInstanceDefFn   = int  (*)(const char* name, const char* def_json);
    using GetInstanceDefFn   = int  (*)(const char* name, char* buf, int buflen);
    using ExchangeInstanceFn = int  (*)(const char* name, const char* cmd_json, char* rsp, int rsplen);
    using SetUseCallbacksFn  = void (*)(void* process_fn, void* exchange_fn,
                                        void* grab_fn, void* host_api);
    using SetTriggerCallbacksFn = void (*)(void* info_fn, void* image_fn,
                                           void* sources_fn);
    using SetRunContextFn         = void (*)(const char* frame_path);
    using SetGlobalCancelFn       = void (*)(int set);

    InspectFn          inspect          = nullptr;
    InspectTvFn        inspect_tv       = nullptr;   // A4 explicit-trigger entry (preferred)
    ListParamsFn       list_params      = nullptr;
    SetParamFn         set_param        = nullptr;
    ResetFn            reset            = nullptr;
    ListInstancesFn    list_instances   = nullptr;
    SetInstanceDefFn   set_instance_def = nullptr;
    GetInstanceDefFn   get_instance_def = nullptr;
    ExchangeInstanceFn exchange_instance = nullptr;
    SetUseCallbacksFn  set_use_callbacks = nullptr;
    SetTriggerCallbacksFn set_trigger_callbacks = nullptr;
    using SetTriggerLeaderCallbackFn = void (*)(void* leader_fn);
    SetTriggerLeaderCallbackFn set_trigger_leader_callback = nullptr;
    // ABI v5: metadata-doc callback (emit_trigger_record). Optional symbol.
    using SetTriggerMetaCallbackFn = void (*)(void* meta_fn);
    SetTriggerMetaCallbackFn set_trigger_meta_callback = nullptr;
    using SetStatusCallbackFn = void (*)(void* fn);
    SetStatusCallbackFn set_status_callback = nullptr;
    // C1: image-pool owner get/set thunks. Optional symbol — older scripts lack
    // it and worker-created images stay anonymous (owner=0).
    using SetOwnerCallbacksFn = void (*)(void* get_fn, void* set_fn);
    SetOwnerCallbacksFn set_owner_callbacks = nullptr;
    // F4: trigger-context marker get/set thunks. Optional symbol — older scripts
    // lack it and off-thread current_trigger() misuse detection is a no-op.
    using SetTriggerCtxCallbacksFn = void (*)(void* get_fn, void* set_fn);
    SetTriggerCtxCallbacksFn set_trigger_ctx_callbacks = nullptr;
    using SetResultCallbackFn = void (*)(void* fn);
    SetResultCallbackFn set_result_callback = nullptr;
    // polaris2 Gate P2: xi::use() pack-door process callback. Optional symbol —
    // older scripts lack it and use(...).process(ScriptPack) yields an empty pack.
    using SetUsePackCallbackFn = void (*)(void* fn);
    SetUsePackCallbackFn set_use_pack_callback = nullptr;
    // polaris2 gate P2 (expose-from-script): use-pack push thunk for
    // xi::use(sink).push(pack). Optional symbol — an older script lacks it
    // and the host simply doesn't wire the pack push.
    using SetUsePushPackCallbackFn = void (*)(void* push_fn);
    SetUsePushPackCallbackFn set_use_push_pack_callback = nullptr;
    SetRunContextFn    set_run_context  = nullptr;
    // U3 (docs/new_gen/17): per-run arrival/run id → the script's xi::run_id().
    // Optional symbol — older scripts lack it and read 0.
    using SetRunIdFn = void (*)(long long run_id);
    SetRunIdFn         set_run_id       = nullptr;
    SetGlobalCancelFn  set_global_cancel = nullptr;
    // Inspect-start hook: draws this inspect's cancel ticket so the watchdog's
    // cooperative cancel is scoped to inspects in flight at trip time, not
    // fresh ones dispatched during the grace. Optional symbol (older scripts
    // lack it → legacy global-cancel behaviour).
    using InspectBeginFn = void (*)(void);
    InspectBeginFn     inspect_begin    = nullptr;

    // U2 (docs/new_gen/16) — the kv channel (post-Record state). Canonical-mp
    // BYTES with explicit lengths (never NUL-terminated). All optional
    // symbols: an older script lacks them and only the Record channel rides.
    using GetKvFn          = int (*)(uint8_t* buf, int buflen);
    using SetKvFn          = int (*)(const uint8_t* bytes, int len);
    using KvSchemaVersionFn = int (*)(void);
    using KvChangeFn       = int (*)(const uint8_t* old_bytes, int old_len,
                                     int old_schema, int new_schema,
                                     uint8_t* buf, int buflen);
    GetKvFn            get_kv            = nullptr;
    SetKvFn            set_kv            = nullptr;
    KvSchemaVersionFn  kv_schema_version = nullptr;
    KvChangeFn         kv_change         = nullptr;

    bool ok() const { return handle && (inspect || inspect_tv); }
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
    out.inspect_tv  = reinterpret_cast<LoadedScript::InspectTvFn>(GetProcAddress(h, "xi_inspect_entry_tv"));
    out.list_params = reinterpret_cast<LoadedScript::ListParamsFn>(GetProcAddress(h, "xi_script_list_params"));
    out.set_param   = reinterpret_cast<LoadedScript::SetParamFn>(GetProcAddress(h, "xi_script_set_param"));
    out.reset            = reinterpret_cast<LoadedScript::ResetFn>(GetProcAddress(h, "xi_script_reset"));
    out.list_instances   = reinterpret_cast<LoadedScript::ListInstancesFn>(GetProcAddress(h, "xi_script_list_instances"));
    out.set_instance_def = reinterpret_cast<LoadedScript::SetInstanceDefFn>(GetProcAddress(h, "xi_script_set_instance_def"));
    out.get_instance_def = reinterpret_cast<LoadedScript::GetInstanceDefFn>(GetProcAddress(h, "xi_script_get_instance_def"));
    out.exchange_instance = reinterpret_cast<LoadedScript::ExchangeInstanceFn>(GetProcAddress(h, "xi_script_exchange_instance"));
    out.set_use_callbacks = reinterpret_cast<LoadedScript::SetUseCallbacksFn>(GetProcAddress(h, "xi_script_set_use_callbacks"));
    out.set_use_push_pack_callback = reinterpret_cast<LoadedScript::SetUsePushPackCallbackFn>(GetProcAddress(h, "xi_script_set_use_push_pack_callback"));
    out.set_trigger_callbacks = reinterpret_cast<LoadedScript::SetTriggerCallbacksFn>(GetProcAddress(h, "xi_script_set_trigger_callbacks"));
    out.set_trigger_leader_callback = reinterpret_cast<LoadedScript::SetTriggerLeaderCallbackFn>(GetProcAddress(h, "xi_script_set_trigger_leader_callback"));
    out.set_trigger_meta_callback = reinterpret_cast<LoadedScript::SetTriggerMetaCallbackFn>(GetProcAddress(h, "xi_script_set_trigger_meta_callback"));
    out.set_status_callback = reinterpret_cast<LoadedScript::SetStatusCallbackFn>(GetProcAddress(h, "xi_script_set_status_callback"));
    out.set_owner_callbacks = reinterpret_cast<LoadedScript::SetOwnerCallbacksFn>(GetProcAddress(h, "xi_script_set_owner_callbacks"));
    out.set_trigger_ctx_callbacks = reinterpret_cast<LoadedScript::SetTriggerCtxCallbacksFn>(GetProcAddress(h, "xi_script_set_trigger_ctx_callbacks"));
    out.set_result_callback = reinterpret_cast<LoadedScript::SetResultCallbackFn>(GetProcAddress(h, "xi_script_set_result_callback"));
    out.set_use_pack_callback = reinterpret_cast<LoadedScript::SetUsePackCallbackFn>(GetProcAddress(h, "xi_script_set_use_pack_callback"));
    out.set_run_context = reinterpret_cast<LoadedScript::SetRunContextFn>(GetProcAddress(h, "xi_script_set_run_context"));
    out.set_run_id      = reinterpret_cast<LoadedScript::SetRunIdFn>(GetProcAddress(h, "xi_script_set_run_id"));
    out.set_global_cancel = reinterpret_cast<LoadedScript::SetGlobalCancelFn>(GetProcAddress(h, "xi_script_set_global_cancel"));
    out.inspect_begin     = reinterpret_cast<LoadedScript::InspectBeginFn>(GetProcAddress(h, "xi_script_inspect_begin"));
    out.get_kv               = reinterpret_cast<LoadedScript::GetKvFn>(GetProcAddress(h, "xi_script_kv_get"));
    out.set_kv               = reinterpret_cast<LoadedScript::SetKvFn>(GetProcAddress(h, "xi_script_kv_set"));
    out.kv_schema_version    = reinterpret_cast<LoadedScript::KvSchemaVersionFn>(GetProcAddress(h, "xi_script_kv_schema_version"));
    out.kv_change            = reinterpret_cast<LoadedScript::KvChangeFn>(GetProcAddress(h, "xi_script_kv_change"));
    if (!out.inspect && !out.inspect_tv) {
        err = "script missing xi_inspect_entry / xi_inspect_entry_tv export";
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
                // Pack-plane analogue (slot bridge; no-op until install_pack_abi):
                // reclaim sealed-pack refs the script retained and never released.
                int fswept = ImagePool::sweep_packs_for(oid);
                if (fswept > 0) {
                    std::fprintf(stderr,
                        "[xinsp2] script unload: swept %d leaked pack ref(s)\n",
                        fswept);
                }
            }
            if (m) FreeLibrary(static_cast<HMODULE>(m));
        } catch (...) { /* deleter must not throw */ }
    });
    return true;
}

// U2 (docs/new_gen/16) — attempt a typed code_change migration of the kv store
// across a hot-reload (the kv channel's whole migrate path; the Record-channel
// migrate_state it once sat beside was removed at THE CUT). `old_bytes`
// is the retiring DLL's serialized kv store (canonical mp; may contain NULs —
// held in a std::string as a plain byte bag). On success fills `out` with the
// migrated bytes for the host to set_kv. Returns false when the hook is absent
// OR declines (0) — the caller then DROPS, exactly like the Record channel.
inline bool migrate_kv(const LoadedScript& s, const std::string& old_bytes,
                       int old_schema, int new_schema, std::string& out) {
    if (!s.kv_change) return false;
    std::vector<uint8_t> buf(64 * 1024);
    int n = s.kv_change((const uint8_t*)old_bytes.data(), (int)old_bytes.size(),
                        old_schema, new_schema, buf.data(), (int)buf.size());
    if (n < 0) {
        buf.resize((size_t)(-(int64_t)n) + 1024);
        n = s.kv_change((const uint8_t*)old_bytes.data(), (int)old_bytes.size(),
                        old_schema, new_schema, buf.data(), (int)buf.size());
    }
    if (n <= 0) return false;   // absent-shaped / declined -> caller drops
    out.assign((const char*)buf.data(), (size_t)n);
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
