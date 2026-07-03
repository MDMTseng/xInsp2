//
// lut_user — the QA consumer side of the resource-handle convention (doc 14
// appendix). Its xi.pack@1 door speaks to the demo.lut TYPE OWNER by
// capability name through the host forwarding funnel — it never sees the
// lut_owner instance, its vtable, or the heavy object itself. Everything that
// crosses this door is either scalar results or the HANDLE ENTRY, the nested
// canonical-mp map {type:"demo.lut", id, gen, $v} that packs carry instead of
// the object (rule: heavy objects never ride packs).
//
// The door composes up to three legs from what the input pack carries:
//
//   build:  mp "bkeys" + mp "bvals"      -> demo.lut.build
//           out: mp "handle" (the provider's handle bytes, forwarded
//           VERBATIM — this door hop is exactly how a handle rides packs),
//           i64 "build_rc" (funnel rc), i64 "built", i64 "builds",
//           str "b_fault" ($fault code or "").
//   query:  mp "handle" (input pack OR the handle just built) + mp "qkeys"
//           -> demo.lut.query
//           out: mp "values", i64 "query_rc", i64 "found", i64 "q_builds"
//           (the owner's build counter echoed at query time — the script's
//           zero-rebuild proof across consumers), str "q_fault".
//   dump:   i64 "dump" != 0 (+ the same handle resolution) -> demo.lut.dump
//           out: bin "lut", i64 "dump_rc", str "d_fault".
//
#include <xi/xi_abi.hpp>

#include <cstring>
#include <string>
#include <vector>

class LutUser : public xi::Plugin {
public:
    LutUser(const xi_host_api* host, const std::string& name)
        : xi::Plugin(host, name) {
        if (host && host->get_interface)
            cap_ = static_cast<const xi_cap_v1*>(host->get_interface("xi.cap", 1));
    }

    using xi::Plugin::process;   // keep the Record overload visible (pack-only override)

    void process(xi::PackIn& in, xi::PackOut& out) override {
        const xi_pack_v1* pk = pack_iface();
        if (!pk || !cap_) {
            out.fault("missing_capability", "xi.cap",
                      "lut_user: host publishes no capability plane");
            return;
        }

        // The handle this door will query/dump with: the input pack's entry,
        // possibly replaced by the one a build leg just minted.
        std::vector<uint8_t> handle;
        if (auto h = in.mp("handle"))
            handle.assign(h->first, h->first + h->second);

        // ---- build leg -----------------------------------------------------
        auto bk = in.mp("bkeys");
        auto bv = in.mp("bvals");
        if (bk && bv) {
            xi_pack_builder b = pk->builder_new();
            pk->builder_add_mp(b, "keys",   bk->first, bk->second);
            pk->builder_add_mp(b, "values", bv->first, bv->second);
            pk->builder_add_i64(b, "$v", 1);
            xi_pack_handle req = pk->builder_seal(b);
            xi_pack_handle rsp = XI_PACK_NULL;
            const int32_t rc = cap_->call("demo.lut.build", req, &rsp);
            pk->release(req);
            out.i64("build_rc", rc);
            out.str("b_fault", fault_of_(pk, rc, rsp));
            if (rc == XI_CAP_OK && rsp != XI_PACK_NULL) {
                const void* hp = nullptr; int32_t hn = 0;
                if (pk->get_mp(rsp, "handle", &hp, &hn) && hp && hn > 0) {
                    // Forward the provider's handle bytes VERBATIM: the entry
                    // now rides THIS door's output pack to the script, which
                    // hops it to the next consumer.
                    out.mp("handle", hp, (size_t)hn);
                    handle.assign(static_cast<const uint8_t*>(hp),
                                  static_cast<const uint8_t*>(hp) + hn);
                }
                int64_t v = -1;
                if (pk->get_i64(rsp, "built", &v))  out.i64("built", v);
                if (pk->get_i64(rsp, "builds", &v)) out.i64("builds", v);
                pk->release(rsp);
            }
        }

        // ---- query leg -----------------------------------------------------
        if (auto qk = in.mp("qkeys")) {
            if (handle.empty()) {
                out.i64("query_rc", -100).str("q_fault", "no_handle");
            } else {
                xi_pack_builder b = pk->builder_new();
                pk->builder_add_mp(b, "handle", handle.data(), (int32_t)handle.size());
                pk->builder_add_mp(b, "keys", qk->first, qk->second);
                pk->builder_add_i64(b, "$v", 1);
                xi_pack_handle req = pk->builder_seal(b);
                xi_pack_handle rsp = XI_PACK_NULL;
                const int32_t rc = cap_->call("demo.lut.query", req, &rsp);
                pk->release(req);
                out.i64("query_rc", rc);
                out.str("q_fault", fault_of_(pk, rc, rsp));
                if (rc == XI_CAP_OK && rsp != XI_PACK_NULL) {
                    const void* vp = nullptr; int32_t vn = 0;
                    if (pk->get_mp(rsp, "values", &vp, &vn) && vp && vn > 0)
                        out.mp("values", vp, (size_t)vn);
                    int64_t v = -1;
                    if (pk->get_i64(rsp, "found", &v))  out.i64("found", v);
                    if (pk->get_i64(rsp, "builds", &v)) out.i64("q_builds", v);
                    pk->release(rsp);
                }
            }
        }

        // ---- dump leg (the persistence materializer) -------------------------
        if (in.i64_or("dump", 0) != 0) {
            if (handle.empty()) {
                out.i64("dump_rc", -100).str("d_fault", "no_handle");
            } else {
                xi_pack_builder b = pk->builder_new();
                pk->builder_add_mp(b, "handle", handle.data(), (int32_t)handle.size());
                pk->builder_add_i64(b, "$v", 1);
                xi_pack_handle req = pk->builder_seal(b);
                xi_pack_handle rsp = XI_PACK_NULL;
                const int32_t rc = cap_->call("demo.lut.dump", req, &rsp);
                pk->release(req);
                out.i64("dump_rc", rc);
                out.str("d_fault", fault_of_(pk, rc, rsp));
                if (rc == XI_CAP_OK && rsp != XI_PACK_NULL) {
                    const void* dp = nullptr; int32_t dn = 0;
                    if (pk->get_bin(rsp, "lut", &dp, &dn) && dp && dn > 0)
                        out.bin("lut", dp, (size_t)dn);
                    pk->release(rsp);
                }
            }
        }
    }

private:
    // The provider's $fault code (or "") out of a capability reply.
    static std::string fault_of_(const xi_pack_v1* pk, int32_t rc, xi_pack_handle rsp) {
        if (rc != XI_CAP_OK || rsp == XI_PACK_NULL) return "";
        const char* p = nullptr; int32_t n = 0;
        if (pk->get_str(rsp, "$fault", &p, &n) && p && n > 0)
            return std::string(p, (size_t)n);
        return "";
    }

    const xi_cap_v1* cap_ = nullptr;
};

XI_PLUGIN_IMPL(LutUser)
XI_PLUGIN_PACK_DOOR(LutUser)
