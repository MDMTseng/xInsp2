//
// lut_client — the consumer half of the lut_owner example.
//
// This is what "using a capability" looks like from inside a plugin. There are
// only three moves, and they are all in the ~40 lines below:
//
//   1. ONCE, in the constructor: ask the host for the consumer vtable.
//          host->get_interface("xi.cap", 1) -> const xi_cap_v1*
//      This is the capability PLANE, not a provider. It exists whether or not
//      anybody is providing anything.
//
//   2. Before using a capability: cap_->available("demo.lut.build"). Cheap
//      existence probe. A capability is optional by construction, so a
//      consumer that does not check has simply decided to crash later.
//
//   3. To use it: build a request pack, cap_->call(<name>, req, &rsp). The
//      host funnel finds whoever registered that name, runs their handler
//      SEH-wrapped, and charges any fault to THEM. We never learn who they
//      are. On XI_CAP_OK we own `rsp` and must release it.
//
// The payload story is the resource-handle convention (docs/new_gen/14
// appendix): the lookup table itself never crosses this ABI. What crosses is
// the HANDLE ENTRY — a nested canonical-mp map {type:"demo.lut", id, gen, $v}
// — which this door copies VERBATIM into its output pack so the script can
// hold it between ticks and hand it back. Bytes we do not interpret.
//
// Door protocol (all legs optional; the script composes what it needs):
//   in  mp "bkeys" + mp "bvals" -> demo.lut.build; out mp "handle",
//                                  i64 "built"/"builds"/"build_rc",
//                                  str "b_fault"
//   in  mp "handle" (+ the one just built) + mp "qkeys" -> demo.lut.query;
//                                  out mp "values", i64 "found"/"query_rc",
//                                  str "q_fault"
//   out i64 "have_cap" — 1 when a demo.lut provider is loaded, else 0 and
//                        nothing else was attempted. This is the whole
//                        graceful-degradation contract.
//
#include <xi/xi_abi.hpp>

#include <string>
#include <vector>

class LutClient : public xi::Plugin {
public:
    LutClient(const xi_host_api* host, const std::string& name)
        : xi::Plugin(host, name) {
        // (1) resolve the capability plane once, at construction.
        if (host && host->get_interface)
            cap_ = static_cast<const xi_cap_v1*>(host->get_interface("xi.cap", 1));
    }

    using xi::Plugin::process;   // keep the Record overload visible

    void process(xi::PackIn& in, xi::PackOut& out) override {
        const xi_pack_v1* pk = pack_iface();
        if (!pk || !cap_) {
            // No capability plane at all: that is a host-shape problem, not an
            // absent provider, so it is a fault rather than a degrade.
            out.fault("missing_capability", "xi.cap",
                      "lut_client: host publishes no capability plane");
            return;
        }

        // (2) is anybody providing demo.lut today? Probed per call, so the
        // answer tracks a provider being added or removed while we run.
        const bool have = cap_->available("demo.lut.build") != 0;
        out.i64("have_cap", have ? 1 : 0);
        if (!have) return;                    // degrade: say so, do nothing else

        // The handle this call will query with: whatever the script handed us,
        // possibly replaced by one the build leg mints below.
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
            pk->builder_add_i64(b, "$v", 1);          // version rides IN the pack
            xi_pack_handle req = pk->builder_seal(b);
            xi_pack_handle rsp = XI_PACK_NULL;
            const int32_t rc = cap_->call("demo.lut.build", req, &rsp);   // (3)
            pk->release(req);
            out.i64("build_rc", rc);
            out.str("b_fault", fault_of_(pk, rc, rsp));
            if (rc == XI_CAP_OK && rsp != XI_PACK_NULL) {
                const void* hp = nullptr; int32_t hn = 0;
                if (pk->get_mp(rsp, "handle", &hp, &hn) && hp && hn > 0) {
                    // Verbatim. We do not parse the handle; it is the owner's
                    // vocabulary, and copying it is how a handle rides a pack.
                    out.mp("handle", hp, (size_t)hn);
                    handle.assign(static_cast<const uint8_t*>(hp),
                                  static_cast<const uint8_t*>(hp) + hn);
                }
                int64_t v = -1;
                if (pk->get_i64(rsp, "built", &v))  out.i64("built", v);
                if (pk->get_i64(rsp, "builds", &v)) out.i64("builds", v);
                pk->release(rsp);              // XI_CAP_OK => we own the reply
            }
        }

        // ---- query leg -----------------------------------------------------
        if (auto qk = in.mp("qkeys")) {
            if (handle.empty()) {
                out.i64("query_rc", -100).str("q_fault", "no_handle");
                return;
            }
            xi_pack_builder b = pk->builder_new();
            pk->builder_add_mp(b, "handle", handle.data(), (int32_t)handle.size());
            pk->builder_add_mp(b, "keys", qk->first, qk->second);
            pk->builder_add_i64(b, "$v", 1);
            xi_pack_handle req = pk->builder_seal(b);
            xi_pack_handle rsp = XI_PACK_NULL;
            const int32_t rc = cap_->call("demo.lut.query", req, &rsp);
            pk->release(req);
            out.i64("query_rc", rc);
            // NOTE: a stale handle is NOT an rc error. rc stays 0 and the
            // provider answers a sealed $fault pack — a contract answer, which
            // the script is expected to handle by rebuilding.
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

private:
    // The provider's $fault code out of a capability reply (or "" when clean).
    static std::string fault_of_(const xi_pack_v1* pk, int32_t rc, xi_pack_handle rsp) {
        if (rc != XI_CAP_OK || rsp == XI_PACK_NULL) return "";
        const char* p = nullptr; int32_t n = 0;
        if (pk->get_str(rsp, "$fault", &p, &n) && p && n > 0)
            return std::string(p, (size_t)n);
        return "";
    }

    const xi_cap_v1* cap_ = nullptr;
};

XI_PLUGIN_IMPL(LutClient)
XI_PLUGIN_PACK_DOOR(LutClient)
