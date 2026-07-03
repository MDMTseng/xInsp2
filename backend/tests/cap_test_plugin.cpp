//
// cap_test_plugin.cpp — a controllable LIB plugin for the capability-plane
// pilot test (test_cap_plane.cpp, docs/new_gen/14). Like fault_plugin it uses
// RAW C exports (no XI_PLUGIN_IMPL) so a hardware fault inside a capability
// handler propagates OUT to the host funnel's SEH boundary exactly as a real
// crashing lib plugin would.
//
// Registers (in its FACTORY — the lifecycle window):
//   * "test.echo"    — echoes i64 "x" back as "x_echo" = x + 1.
//   * "test.ver"     — the $v/$probe convention exemplar: supports $v == 1
//                      only; absent $v defaults to 1; $probe: true answers
//                      {$versions: "1"} with no work; an unsupported $v is a
//                      sealed $fault pack naming the supported range.
//   * "test.crash"   — genuine ACCESS_VIOLATION inside the handler.
//   * "test.reenter" — calls BACK into the capability plane targeting
//                      "test.echo" (provided by THIS SAME instance) — the
//                      funnel must refuse with XI_CAP_EREENTRY (-5) — and also
//                      attempts a REGISTRATION from inside the handler (must
//                      be refused with XI_CAP_REG_ECONTEXT). Reports both rcs
//                      in its output pack.
//
// The exports also cover the negative registration windows:
//   * xi_plugin_process attempts a registration from inside the DATA-PLANE
//     door (must be XI_CAP_REG_ECONTEXT); rc readable via exchange.
//   * exchange "unregister_echo" drops "test.echo" from lifecycle code
//     (legal), proving targeted unregister; the REMAINING registrations are
//     deliberately NOT unregistered on destroy, so the adapter-dtor sweep is
//     what the test observes.
//
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <xi/xi_abi.h>
#include <xi/xi_record.hpp>   // xi::yyjson_layout_stamp()

namespace {

struct CapTestInstance {
    const xi_host_api*         host     = nullptr;
    const xi_pack_v1*          pack     = nullptr;   // build/read packs
    const xi_cap_v1*           cap      = nullptr;   // consumer side (reenter)
    const xi_cap_provider_v1*  provider = nullptr;   // registration side
    int  reg_rc_echo    = -999;  // factory registration results
    int  reg_rc_ver     = -999;
    int  reg_rc_crash   = -999;
    int  reg_rc_reenter = -999;
    int  reg_rc_dup     = -999;  // same-owner duplicate (must overwrite: OK)
    int  reg_in_process_rc = -999;  // set by xi_plugin_process (must be ECONTEXT)
    std::atomic<int> echo_calls{0};
};

// ---- handlers (pack-door-shaped: sealed pack in, NEW sealed pack out) ------

xi_pack_handle h_echo(void* self, xi_pack_handle in) {
    auto* i = static_cast<CapTestInstance*>(self);
    if (!i || !i->pack) return XI_PACK_NULL;
    i->echo_calls.fetch_add(1, std::memory_order_relaxed);
    int64_t x = 0;
    i->pack->get_i64(in, "x", &x);
    xi_pack_builder b = i->pack->builder_new();
    i->pack->builder_add_i64(b, "x_echo", x + 1);
    return i->pack->builder_seal(b);
}

// The $v/$probe convention exemplar (doc 14 name-only registry refinement).
xi_pack_handle h_ver(void* self, xi_pack_handle in) {
    auto* i = static_cast<CapTestInstance*>(self);
    if (!i || !i->pack) return XI_PACK_NULL;
    // $probe: true → answer the supported versions, do NO work.
    int32_t probe = 0;
    if (i->pack->get_bool && i->pack->get_bool(in, "$probe", &probe) && probe) {
        xi_pack_builder b = i->pack->builder_new();
        i->pack->builder_add_str(b, "$versions", "1", 1);
        return i->pack->builder_seal(b);
    }
    // $v absent → the documented default (1); unsupported → sealed $fault.
    int64_t v = 1;
    i->pack->get_i64(in, "$v", &v);
    if (v != 1) {
        xi_pack_builder b = i->pack->builder_new();
        i->pack->builder_add_str(b, "$fault", "unsupported_version", 19);
        i->pack->builder_add_str(b, "$fault_key", "$v", 2);
        i->pack->builder_add_str(b, "$versions", "1", 1);
        return i->pack->builder_seal(b);
    }
    xi_pack_builder b = i->pack->builder_new();
    i->pack->builder_add_i64(b, "v_used", v);
    return i->pack->builder_seal(b);
}

xi_pack_handle h_crash(void* /*self*/, xi_pack_handle /*in*/) {
    volatile int* np = nullptr;
    *np = 42;   // ACCESS_VIOLATION → seh_exception at the host funnel boundary
    return XI_PACK_NULL;
}

xi_pack_handle h_reenter(void* self, xi_pack_handle in) {
    auto* i = static_cast<CapTestInstance*>(self);
    if (!i || !i->pack || !i->cap) return XI_PACK_NULL;
    // Call BACK into the plane, targeting a capability THIS instance provides
    // — the per-thread acyclicity guard must answer XI_CAP_EREENTRY.
    xi_pack_handle echoed = XI_PACK_NULL;
    int32_t rc = i->cap->call("test.echo", in, &echoed);
    if (echoed != XI_PACK_NULL) i->pack->release(echoed);   // must not happen
    // And try to REGISTER from inside a handler (data plane) — must be refused.
    int32_t reg_rc = -999;
    if (i->provider)
        reg_rc = i->provider->register_capability("test.sneaky", &h_echo, self);
    xi_pack_builder b = i->pack->builder_new();
    i->pack->builder_add_i64(b, "reenter_rc", rc);
    i->pack->builder_add_i64(b, "reg_in_handler_rc", reg_rc);
    return i->pack->builder_seal(b);
}

} // namespace

extern "C" {

__declspec(dllexport) int xi_plugin_abi_version(void) { return XI_ABI_MIN_COMPAT; }
__declspec(dllexport) uint32_t xi_yyjson_abi(void) { return xi::yyjson_layout_stamp(); }

__declspec(dllexport) void* xi_plugin_create(const xi_host_api* host, const char* /*name*/) {
    auto* i = new CapTestInstance();
    i->host = host;
    if (host && host->get_interface) {
        i->pack     = static_cast<const xi_pack_v1*>(host->get_interface("xi.pack", 1));
        i->cap      = static_cast<const xi_cap_v1*>(host->get_interface("xi.cap", 1));
        i->provider = static_cast<const xi_cap_provider_v1*>(
                          host->get_interface("xi.cap.provider", 1));
    }
    if (i->provider) {
        // The lifecycle registration window: the factory runs under the host's
        // owner context, so these are attributed to this instance.
        i->reg_rc_echo    = i->provider->register_capability("test.echo",    &h_echo,    i);
        i->reg_rc_ver     = i->provider->register_capability("test.ver",     &h_ver,     i);
        i->reg_rc_crash   = i->provider->register_capability("test.crash",   &h_crash,   i);
        i->reg_rc_reenter = i->provider->register_capability("test.reenter", &h_reenter, i);
        // Same-owner duplicate = overwrite (the reinit re-register path): OK.
        i->reg_rc_dup     = i->provider->register_capability("test.echo",    &h_echo,    i);
    }
    return i;
}

// Deliberately NO unregister here: the adapter-dtor owner sweep is under test.
__declspec(dllexport) void xi_plugin_destroy(void* p) {
    delete static_cast<CapTestInstance*>(p);
}

// The DATA-PLANE registration probe: registering from inside process() must be
// refused with XI_CAP_REG_ECONTEXT (the adapter's door carries the data-plane
// mark). Result readable via exchange.
__declspec(dllexport) void xi_plugin_process(void* p, const xi_record* /*in*/,
                                             xi_record_out* /*out*/) {
    auto* i = static_cast<CapTestInstance*>(p);
    if (!i || !i->provider) return;
    i->reg_in_process_rc =
        i->provider->register_capability("test.from_process", &h_echo, i);
}

__declspec(dllexport) int xi_plugin_get_def(void* /*p*/, char* buf, int cap) {
    return std::snprintf(buf, (size_t)cap, "{}");
}
__declspec(dllexport) int xi_plugin_set_def(void* /*p*/, const char* /*json*/) { return 0; }

__declspec(dllexport) int xi_plugin_exchange(void* p, const char* cmd, char* buf, int cap) {
    auto* i = static_cast<CapTestInstance*>(p);
    if (!i || !cmd) return 0;
    int unreg_rc = -999;
    if (std::strstr(cmd, "unregister_echo") && i->provider)
        unreg_rc = i->provider->unregister_capability("test.echo", i);
    return std::snprintf(buf, (size_t)cap,
        "{\"reg_echo\":%d,\"reg_ver\":%d,\"reg_crash\":%d,\"reg_reenter\":%d,"
        "\"reg_dup\":%d,\"reg_in_process\":%d,\"unreg_echo\":%d,\"echo_calls\":%d}",
        i->reg_rc_echo, i->reg_rc_ver, i->reg_rc_crash, i->reg_rc_reenter,
        i->reg_rc_dup, i->reg_in_process_rc, unreg_rc,
        i->echo_calls.load(std::memory_order_relaxed));
}

} // extern "C"
