//
// test_state_migrate.cpp — G4 / OQ-5 "code_change-style state-migration hook".
//
// Proves the opt-in migration hook the host consults on a state-schema mismatch
// during hot-reload (service_main.cpp compile_and_load restore block), driven
// through the REAL loader (xi::script::load_script) + the REAL host decision
// helper (xi::script::migrate_state) — exactly the two pieces service_main uses.
//
//   SECTION A (hook present)  — new DLL exports xi_script_code_change: on a
//     v1 -> v2 schema change the host MIGRATES the prior state forward (the old
//     bytes appear, re-shaped) instead of dropping it.
//   SECTION B (hook absent)   — new DLL has no xi_script_code_change: the host
//     helper declines, so the pre-G4 DROP-on-mismatch behaviour is preserved.
//   SECTION C (grow-and-retry) — migrate_state honours get_state's negative
//     "buffer too small" return by re-calling with a larger buffer.
//
// Fixtures (migrate_probe.cpp built 3 ways) use raw C exports like reload_probe.
//

#include <xi/xi_script_loader.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef MIGRATE_PROBE_V1_DLL
#define MIGRATE_PROBE_V1_DLL        "migrate_probe_v1.dll"
#endif
#ifndef MIGRATE_PROBE_V2_DLL
#define MIGRATE_PROBE_V2_DLL        "migrate_probe_v2.dll"
#endif
#ifndef MIGRATE_PROBE_V2_NOHOOK_DLL
#define MIGRATE_PROBE_V2_NOHOOK_DLL "migrate_probe_v2_nohook.dll"
#endif

#define CHECK(expr)                                                  \
    do {                                                             \
        if (!(expr)) {                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                 \
                __FILE__, __LINE__, #expr);                          \
            std::abort();                                            \
        }                                                            \
    } while (0)

#define SECTION(name) std::fprintf(stderr, "-- %s\n", name)

using xi::script::LoadedScript;

static LoadedScript load(const char* path) {
    LoadedScript s;
    std::string err;
    if (!xi::script::load_script(path, s, err)) {
        std::fprintf(stderr, "FAIL: load_script(%s): %s\n", path, err.c_str());
        std::abort();
    }
    CHECK(s.ok());
    return s;
}

// Read the live state back out of a loaded fixture (grow-and-retry convention).
static std::string read_state(const LoadedScript& s) {
    std::string out(4096, '\0');
    int n = s.get_state(out.data(), (int)out.size());
    if (n < 0) { out.resize((size_t)(-n) + 16);
                 n = s.get_state(out.data(), (int)out.size()); }
    CHECK(n >= 0);
    out.resize((size_t)n);
    return out;
}

// Mirror of service_main.cpp's drop predicate: drop candidate when the new DLL
// declares a schema and it differs from the persisted one.
static bool is_mismatch(int old_schema, int new_schema) {
    return new_schema != 0 && old_schema != new_schema;
}

int main() {
    std::fprintf(stderr, "=== test_state_migrate ===\n");

    // --- capture the "old" DLL's persisted state, as the host does pre-swap ---
    LoadedScript v1 = load(MIGRATE_PROBE_V1_DLL);
    const std::string old_json = read_state(v1);   // seeded {"count":7}
    const int old_schema = v1.state_schema_version ? v1.state_schema_version() : 0;
    CHECK(old_schema == 1);
    CHECK(old_json.find("\"count\":7") != std::string::npos);
    xi::script::unload_script(v1);

    // ---------------------------------------------------------------- SECTION A
    SECTION("A: hook present -> state migrates across the schema change");
    {
        LoadedScript v2 = load(MIGRATE_PROBE_V2_DLL);
        const int new_schema = v2.state_schema_version();
        CHECK(new_schema == 2);
        CHECK(is_mismatch(old_schema, new_schema));   // would drop pre-G4
        CHECK(v2.code_change != nullptr);             // hook detected by loader

        std::string migrated;
        bool ok = xi::script::migrate_state(v2, old_json, old_schema, new_schema, migrated);
        CHECK(ok);                                    // migrated, NOT dropped
        // The old bytes were carried forward, re-shaped for the new schema.
        CHECK(migrated.find("\"migrated_from\":1") != std::string::npos);
        CHECK(migrated.find("\"count\":7")         != std::string::npos);

        // Host then restores the migrated shape into the new DLL.
        CHECK(v2.set_state(migrated.c_str()) == 0);
        CHECK(read_state(v2) == migrated);
        xi::script::unload_script(v2);
        std::fprintf(stderr, "   migrated = %s\n", migrated.c_str());
    }

    // ---------------------------------------------------------------- SECTION B
    SECTION("B: hook absent -> drop-on-mismatch preserved (unchanged behaviour)");
    {
        LoadedScript v2 = load(MIGRATE_PROBE_V2_NOHOOK_DLL);
        const int new_schema = v2.state_schema_version();
        CHECK(new_schema == 2);
        CHECK(is_mismatch(old_schema, new_schema));
        CHECK(v2.code_change == nullptr);             // no hook exported

        std::string migrated;
        bool ok = xi::script::migrate_state(v2, old_json, old_schema, new_schema, migrated);
        CHECK(!ok);                                   // declines -> host drops
        CHECK(migrated.empty());
        xi::script::unload_script(v2);
    }

    // ---------------------------------------------------------------- SECTION C
    SECTION("C: migrate_state honours the grow-and-retry buffer convention");
    {
        // A large old_json forces the hook's first (64 KiB) buffer to overflow
        // so migrate_state must re-call with a bigger one.
        std::string big = "{\"blob\":\"";
        big.append(200 * 1024, 'x');
        big += "\"}";

        LoadedScript v2 = load(MIGRATE_PROBE_V2_DLL);
        std::string migrated;
        bool ok = xi::script::migrate_state(v2, big, 1, 2, migrated);
        CHECK(ok);
        CHECK(migrated.size() > big.size());          // wrapped prior + markers
        CHECK(migrated.find(big) != std::string::npos);
        xi::script::unload_script(v2);
    }

    std::fprintf(stderr, "\nALL TESTS PASSED\n");
    return 0;
}
