//
// kv_probe.cpp — fixture "script" DLL for the U2 kv-channel hot-reload test
// (test_kv_migrate.cpp). The kv sibling of migrate_probe.cpp — but where that
// fixture hand-rolls its exports, this one exports the REAL SDK thunk bodies
// (xi::detail::kv_*_thunk from xi_kv.hpp) over the REAL xi::kv() store, so the
// loader test exercises the exact logic xi_script_support.hpp ships, without
// force-including the whole script world (xi_kv.hpp is header-only, std +
// xi_mp.hpp, nothing else).
//
// Built three ways (see backend/CMakeLists.txt):
//   KVP_SCHEMA=1              -> kv_probe_v1.dll        (old DLL, no migrator)
//   KVP_SCHEMA=2, KVP_HOOK    -> kv_probe_v2.dll        (new DLL, migrates)
//   KVP_SCHEMA=2              -> kv_probe_v2_nohook.dll (new DLL, declines)
//
// inspect() behaviour (all builds):
//   * JSON-era SELF-SEED (the doc-16 bilingual-window port pattern): if the
//     kv store has no "count" but the Record-channel stand-in (g_state_json,
//     restored via the raw xi_script_set_state export below) carries one,
//     seed kv from it — then increment. This is what a real porting script
//     does with xi::state()["count"].as_int(0); the fixture extracts the int
//     crudely since it deliberately doesn't link yyjson.
//   * every call: count += 1; and (once) a str + f64 + nested-mp entry so the
//     boundary round-trip covers every slot family.
//
// The KVP_HOOK migrator renames "count" -> "frames" and stamps
// "migrated_from", carrying every other entry across unchanged — so the test
// can prove the old store was re-shaped, not dropped.
//

#include <xi/xi_kv.hpp>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#ifndef KVP_SCHEMA
#define KVP_SCHEMA 1
#endif

XI_KV_SCHEMA(KVP_SCHEMA);

// Record-channel stand-in for the JSON-era self-seed section (SECTION E).
static std::string g_state_json = "{}";

// Crude "count" extractor for the fixture's self-seed (a real script reads the
// already-parsed xi::state() instead; no JSON parser needed here).
static long long extract_count_(const std::string& json) {
    size_t p = json.find("\"count\":");
    if (p == std::string::npos) return -1;
    return std::strtoll(json.c_str() + p + 8, nullptr, 10);
}

#ifdef KVP_HOOK
// Opt-in typed migrator, registered at static-init (same discipline as
// XI_KV_SCHEMA): rename count->frames, stamp provenance, carry the rest.
namespace {
static int _kvp_mig_init = [] {
    xi::set_kv_migrate([](const xi::Kv& old, int from, int /*to*/)
                           -> std::optional<xi::Kv> {
        xi::Kv out;
        for (const std::string& k : old.keys()) {
            if (k == "count") continue;
            switch (old.type_of(k)) {
                case xi::Kv::Type::I64:  out.set_i64(k, old.get_i64(k)); break;
                case xi::Kv::Type::F64:  out.set_f64(k, old.get_f64(k)); break;
                case xi::Kv::Type::Bool: out.set_bool(k, old.get_bool(k)); break;
                case xi::Kv::Type::Str:  out.set_str(k, old.get_str(k)); break;
                case xi::Kv::Type::Bin: {
                    const xi::mp::Bytes* b = old.get_bin(k);
                    if (b) out.set_bin(k, b->data(), b->size());
                    break;
                }
                case xi::Kv::Type::Mp: {
                    const xi::mp::Bytes* b = old.get_mp(k);
                    if (b) out.set_mp(k, b->data(), b->size());
                    break;
                }
            }
        }
        out.set_i64("frames", old.get_i64("count", 0));
        out.set_i64("migrated_from", from);
        return out;
    });
    return 0;
}();
} // namespace
#endif

extern "C" {

// Mandatory export (load_script fails without it).
__declspec(dllexport) void xi_inspect_entry(int /*frame*/) {
    std::lock_guard<std::mutex> lk(xi::kv_mutex());
    if (!xi::kv().has("count")) {
        long long seeded = extract_count_(g_state_json);
        if (seeded >= 0) xi::kv().set_i64("count", seeded);
    }
    xi::kv().set_i64("count", xi::kv().get_i64("count", 0) + 1);
    if (!xi::kv().has("label")) {
        xi::kv().set_str("label", "probe");
        xi::kv().set_f64("ratio", 0.5);
        xi::mp::Writer w;
        w.array(2);
        w.map(1); w.key("x"); w.int_(3);
        w.map(1); w.key("x"); w.int_(4);
        xi::kv().set_mp("pts", w);
    }
}

// ---- the four kv exports: the REAL thunk bodies -----------------------------

__declspec(dllexport) int xi_script_kv_get(uint8_t* buf, int buflen) {
    return xi::detail::kv_get_thunk(buf, buflen);
}
__declspec(dllexport) int xi_script_kv_set(const uint8_t* bytes, int len) {
    return xi::detail::kv_set_thunk(bytes, len);
}
__declspec(dllexport) int xi_script_kv_schema_version(void) {
    return xi::kv_schema_version();
}
__declspec(dllexport) int xi_script_kv_change(const uint8_t* old_bytes, int old_len,
                                              int old_schema, int new_schema,
                                              uint8_t* buf, int buflen) {
    return xi::detail::kv_change_thunk(old_bytes, old_len, old_schema,
                                       new_schema, buf, buflen);
}

// [v12 THE CUT — the Record-channel stand-in exports (xi_script_get_state/
//  set_state) were removed with test_kv_migrate SECTION E (the JSON-era
//  self-seed died with the Record state channel; doc 16).]
