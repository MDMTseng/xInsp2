//
// test_kv_migrate.cpp — U2 kv-channel hot-reload carry (docs/new_gen/16).
//
// The kv sibling of test_state_migrate.cpp: drives the REAL loader
// (xi::script::load_script) + the REAL host decision helper
// (xi::script::migrate_kv) — the exact two pieces service_cmd_lifecycle uses —
// against kv_probe.cpp built three ways. The fixture exports the REAL SDK
// thunk bodies (xi::detail::kv_*_thunk), so this covers the production
// get/set/schema/change logic end to end at the module boundary.
//
//   SECTION A (round-trip)      — bytes captured from the old DLL restore into
//     a fresh DLL byte-identically (get -> set -> get equality), and the test
//     parses the boundary bytes with xi::Kv to verify every slot family
//     (i64 / f64 / str / nested-mp) crossed intact.
//   SECTION B (hook present)    — v1 -> v2 schema mismatch with a registered
//     xi::set_kv_migrate: the store MIGRATES (count -> frames, provenance
//     stamped, other entries carried) instead of dropping.
//   SECTION C (hook declines)   — v2 DLL without a migrator: kv_change is
//     exported (always is) but returns 0, so migrate_kv declines and the host
//     drop path is preserved.
//   SECTION D (grow-and-retry)  — a >64 KiB store forces migrate_kv's negative
//     "-needed" retry, and the migrated bytes survive intact.
//   SECTION E (JSON-era seed)   — the doc-16 bilingual-window port pattern:
//     the host restores the RECORD channel (set_state JSON) as it always did;
//     the script's first inspect self-seeds xi::kv() from it; the kv channel
//     carries from then on. No host cross-codec logic anywhere.
//

#include <xi/xi_kv.hpp>
#include <xi/xi_script_loader.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef KV_PROBE_V1_DLL
#define KV_PROBE_V1_DLL        "kv_probe_v1.dll"
#endif
#ifndef KV_PROBE_V2_DLL
#define KV_PROBE_V2_DLL        "kv_probe_v2.dll"
#endif
#ifndef KV_PROBE_V2_NOHOOK_DLL
#define KV_PROBE_V2_NOHOOK_DLL "kv_probe_v2_nohook.dll"
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

// Read the live kv store back out of a loaded fixture (byte-length +
// grow-and-retry convention; "" = empty store).
static std::string read_kv(const LoadedScript& s) {
    CHECK(s.get_kv != nullptr);
    std::vector<uint8_t> buf(4096);
    int n = s.get_kv(buf.data(), (int)buf.size());
    if (n < 0) { buf.resize((size_t)(-(int64_t)n));
                 n = s.get_kv(buf.data(), (int)buf.size()); }
    CHECK(n >= 0);
    return std::string((const char*)buf.data(), (size_t)n);
}

static xi::Kv parse_kv(const std::string& bytes) {
    xi::Kv kv;
    CHECK(kv.parse((const uint8_t*)bytes.data(), bytes.size()));
    return kv;
}

// Mirror of the host's drop predicate (service_cmd_lifecycle, both channels).
static bool is_mismatch(int old_schema, int new_schema) {
    return new_schema != 0 && old_schema != new_schema;
}

int main() {
    std::fprintf(stderr, "=== test_kv_migrate ===\n");

    // --- capture the "old" DLL's kv store, as the host does pre-swap --------
    LoadedScript v1 = load(KV_PROBE_V1_DLL);
    CHECK(v1.get_kv && v1.set_kv && v1.kv_schema_version && v1.kv_change);
    CHECK(read_kv(v1).empty());          // fresh store => get returns 0 bytes
    v1.inspect(0); v1.inspect(1); v1.inspect(2);
    const std::string old_bytes = read_kv(v1);
    CHECK(!old_bytes.empty());
    const int old_schema = v1.kv_schema_version();
    CHECK(old_schema == 1);
    {   // every slot family crossed the boundary intact
        xi::Kv kv = parse_kv(old_bytes);
        CHECK(kv.get_i64("count") == 3);
        CHECK(kv.get_str("label") == "probe");
        CHECK(kv.get_f64("ratio") == 0.5);
        CHECK(kv.get_mp("pts") != nullptr);
    }
    xi::script::unload_script(v1);

    // ---------------------------------------------------------------- SECTION A
    SECTION("A: boundary bytes round-trip byte-identically into a fresh DLL");
    {
        LoadedScript v1b = load(KV_PROBE_V1_DLL);
        CHECK(read_kv(v1b).empty());                   // truly a fresh store
        CHECK(v1b.set_kv((const uint8_t*)old_bytes.data(), (int)old_bytes.size()) == 0);
        CHECK(read_kv(v1b) == old_bytes);              // deterministic encoding
        // and the restored store keeps counting from where it left off
        v1b.inspect(3);
        CHECK(parse_kv(read_kv(v1b)).get_i64("count") == 4);
        // hostile bytes are refused without touching the restored store
        const uint8_t junk[] = {0xc1, 0x00};
        CHECK(v1b.set_kv(junk, (int)sizeof junk) == -1);
        CHECK(parse_kv(read_kv(v1b)).get_i64("count") == 4);
        xi::script::unload_script(v1b);
    }

    // ---------------------------------------------------------------- SECTION B
    SECTION("B: migrator present -> store migrates across the schema change");
    {
        LoadedScript v2 = load(KV_PROBE_V2_DLL);
        const int new_schema = v2.kv_schema_version();
        CHECK(new_schema == 2);
        CHECK(is_mismatch(old_schema, new_schema));    // would drop without a hook

        std::string migrated;
        bool ok = xi::script::migrate_kv(v2, old_bytes, old_schema, new_schema, migrated);
        CHECK(ok);                                     // migrated, NOT dropped
        xi::Kv kv = parse_kv(migrated);
        CHECK(kv.get_i64("frames") == 3);              // count carried, renamed
        CHECK(!kv.has("count"));
        CHECK(kv.get_i64("migrated_from") == 1);
        CHECK(kv.get_str("label") == "probe");         // other entries carried
        CHECK(kv.get_f64("ratio") == 0.5);
        CHECK(kv.get_mp("pts") != nullptr);

        // host then restores the migrated shape into the new DLL
        CHECK(v2.set_kv((const uint8_t*)migrated.data(), (int)migrated.size()) == 0);
        CHECK(read_kv(v2) == migrated);
        xi::script::unload_script(v2);
        std::fprintf(stderr, "   migrated = %s\n", kv.debug_text().c_str());
    }

    // ---------------------------------------------------------------- SECTION C
    SECTION("C: no migrator -> kv_change declines -> host drop path preserved");
    {
        LoadedScript v2 = load(KV_PROBE_V2_NOHOOK_DLL);
        CHECK(v2.kv_schema_version() == 2);
        CHECK(is_mismatch(old_schema, v2.kv_schema_version()));
        CHECK(v2.kv_change != nullptr);                // export ALWAYS present...
        std::string migrated;
        CHECK(!xi::script::migrate_kv(v2, old_bytes, old_schema, 2, migrated));
        CHECK(migrated.empty());                       // ...but declines (0) -> drop
        xi::script::unload_script(v2);
    }

    // ---------------------------------------------------------------- SECTION D
    SECTION("D: migrate_kv honours the grow-and-retry buffer convention");
    {
        // A >64 KiB store forces migrate_kv's first buffer to overflow so it
        // must re-call with the reported size.
        xi::Kv big;
        std::vector<uint8_t> blob(200 * 1024, 0xAB);
        big.set_bin("blob", blob.data(), blob.size());
        big.set_i64("count", 7);
        xi::mp::Bytes big_bytes = big.serialize();
        std::string big_str((const char*)big_bytes.data(), big_bytes.size());

        LoadedScript v2 = load(KV_PROBE_V2_DLL);
        std::string migrated;
        CHECK(xi::script::migrate_kv(v2, big_str, 1, 2, migrated));
        CHECK(migrated.size() > blob.size());          // the blob rode along
        xi::Kv kv = parse_kv(migrated);
        CHECK(kv.get_i64("frames") == 7);
        const xi::mp::Bytes* carried = kv.get_bin("blob");
        CHECK(carried && carried->size() == blob.size() && *carried == blob);
        xi::script::unload_script(v2);
    }

    // [v12 THE CUT — SECTION E (JSON-era Record→kv self-seed, the bilingual-
    //  window port pattern) was REMOVED: the xi::state() Record channel and
    //  LoadedScript::set_state are deleted, so the self-seed no longer exists
    //  (doc 16 §4.3 — the seed line dies with the Record channel). Sections
    //  A–D (kv round-trip + hook-present migrate + hook-absent drop + grow-
    //  and-retry) remain the migration coverage.]

    std::fprintf(stderr, "\nALL TESTS PASSED\n");
    return 0;
}
