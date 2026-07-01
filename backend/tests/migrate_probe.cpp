//
// migrate_probe.cpp — fixture "script" DLL for the G4/OQ-5 state-migration hook
// test (test_state_migrate.cpp). Like reload_probe.cpp it is a raw-C-export
// minimal loadable script: it implements exactly the xi_script_* exports the
// host loader (xi_script_loader.hpp) resolves for persistent-state handling —
//   xi_inspect_entry              (the one mandatory export)
//   xi_script_get_state           (grow-and-retry buffer convention)
//   xi_script_set_state           (stores the JSON verbatim)
//   xi_script_state_schema_version (compile-time MIG_SCHEMA)
//   xi_script_code_change          (only compiled when MIG_HOOK is defined)
//
// Built three ways from this one source (see backend/CMakeLists.txt):
//   MIG_SCHEMA=1                -> migrate_probe_v1.dll        (old DLL, no hook)
//   MIG_SCHEMA=2, MIG_HOOK      -> migrate_probe_v2.dll        (new DLL, migrates)
//   MIG_SCHEMA=2                -> migrate_probe_v2_nohook.dll (new DLL, no hook)
//
// The hook migrates by wrapping the prior state so the test can prove the old
// bytes were carried forward, not dropped:  {"migrated_from":<old_schema>,"prior":<old_json>}
//

#include <cstring>
#include <string>

#ifndef MIG_SCHEMA
#define MIG_SCHEMA 1
#endif

// Module-global "persistent state" for THIS loaded image. Seeded so a fresh v1
// load already has state to hand back via get_state (stands in for a script that
// accumulated state over prior frames).
static std::string g_state = "{\"count\":7}";

static int emit(const std::string& s, char* buf, int buflen) {
    int needed = (int)s.size();
    if (buflen < needed + 1) return -needed;   // grow-and-retry, like the real thunk
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = 0;
    return needed;
}

extern "C" {

// Mandatory export (load_script fails without it).
__declspec(dllexport) void xi_inspect_entry(int /*frame*/) {}

__declspec(dllexport) int xi_script_get_state(char* buf, int buflen) {
    return emit(g_state, buf, buflen);
}

__declspec(dllexport) int xi_script_set_state(const char* json) {
    if (!json) return -1;
    g_state = json;
    return 0;
}

__declspec(dllexport) int xi_script_state_schema_version(void) {
    return MIG_SCHEMA;
}

#ifdef MIG_HOOK
// The opt-in code_change hook: carry prior state forward instead of dropping.
__declspec(dllexport) int xi_script_code_change(const char* old_json, int old_schema,
                                                int /*new_schema*/, char* buf, int buflen) {
    std::string prior = old_json ? old_json : "{}";
    std::string out = "{\"migrated_from\":" + std::to_string(old_schema)
                    + ",\"prior\":" + prior + "}";
    return emit(out, buf, buflen);
}
#endif

} // extern "C"
