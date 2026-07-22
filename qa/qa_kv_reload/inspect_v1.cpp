//
// qa_kv_reload (v1) — cross-frame state on the POST-RECORD channel:
// xi::kv() (docs/new_gen/16). The kv port of hot_reload_run2's counter
// pattern, with the output surfaced pack-only (ScriptPackBuilder -> expose
// push) so no xi::Record appears anywhere in this script.
//
// v1 counts frames in xi::kv()["count"] under schema 1. The driver hot-swaps
// to v2 (schema 2 + a registered xi::set_kv_migrate) mid-run and asserts the
// count carries across the reload through the host's kv capture/restore legs.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>

#include <mutex>

XI_KV_SCHEMA(1);

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    long long next;
    {
        std::lock_guard<std::mutex> lk(xi::kv_mutex());
        next = xi::kv().get_i64("count", 0) + 1;
        xi::kv().set_i64("count", next);
    }

    xi::ScriptPackBuilder b;
    b.add_str("$channel", "kvqa");
    b.add_i64("count", next);
    b.add_i64("version", 1);
    auto pack = b.seal();
    if (pack) xi::use("expose").push(pack);
}
