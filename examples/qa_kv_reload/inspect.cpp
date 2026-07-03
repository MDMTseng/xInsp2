//
// qa_kv_reload (v2) — the schema-changed successor of inspect_v1.cpp: the
// counter is RENAMED count -> frames (schema 1 -> 2), and the registered
// xi::set_kv_migrate carries the prior store across the mismatch instead of
// letting the host drop it (event:state_migrated with "store":"kv").
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>

#include <mutex>
#include <optional>

XI_KV_SCHEMA(2);

// Registered at static-init (DLL load), BEFORE the host's restore leg runs —
// the same discipline as XI_KV_SCHEMA itself.
namespace {
static int _mig = [] {
    xi::set_kv_migrate([](const xi::Kv& old, int from, int /*to*/)
                           -> std::optional<xi::Kv> {
        xi::Kv out;
        out.set_i64("frames", old.get_i64("count", 0));   // rename, carry value
        out.set_i64("migrated_from", from);
        return out;
    });
    return 0;
}();
} // namespace

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    long long next, mig_from;
    {
        std::lock_guard<std::mutex> lk(xi::kv_mutex());
        next = xi::kv().get_i64("frames", 0) + 1;
        xi::kv().set_i64("frames", next);
        mig_from = xi::kv().get_i64("migrated_from", 0);
    }

    xi::ScriptPackBuilder b;
    b.add_str("$channel", "kvqa");
    b.add_i64("frames", next);
    b.add_i64("version", 2);
    b.add_i64("migrated_from", mig_from);
    auto pack = b.seal();
    if (pack) xi::use("expose").push(pack);
}
