#pragma once
//
// xi_comms.hpp — talk to the PLC (or any external comms) from an inspection
// script, via the out-of-process comms gateway. The backend holds the gateway
// connection; these forward to it through host callbacks. Opt-in header (like
// xi_status.hpp / xi_breakpoint.hpp). See docs/design/comms-gateway.md.
//
//   xi::comms::send(line)        send a line to the PLC; false if no gateway
//   xi::comms::poll()            drain PLC-originated lines since the last call
//   xi::comms::up()              is the PLC link currently up?
//   xi::comms::set_deadman(line) register an emergency line the gateway sends to
//                                the PLC if THIS backend crashes (drops without a
//                                clean shutdown). Call once, e.g. in reset().
//
// The gateway is schema-agnostic: `line` is whatever your PLC speaks (JSON,
// msgpack-as-text, ...). With no gateway configured, all calls are safe no-ops.
//
//   #include <xi/xi.hpp>
//   #include <xi/xi_comms.hpp>
//   void xi_inspect_entry(int frame) {
//       xi::comms::set_deadman("{\"cmd\":\"estop\"}");
//       for (auto& msg : xi::comms::poll()) handle(msg);    // e.g. a trigger/recipe
//       xi::comms::send("{\"frame\":" + std::to_string(frame) + ",\"result\":\"PASS\"}");
//   }
//
#include <string>
#include <vector>

// Defined (static) in xi_script_support.hpp, force-included into every script.
extern void* g_comms_send_fn_;
extern void* g_comms_poll_fn_;
extern void* g_comms_up_fn_;
extern void* g_comms_deadman_fn_;

namespace xi {
namespace comms {

using SendFn    = int (*)(const char* line);   // 1 ok, 0 fail
using PollFn    = int (*)(char* buf, int n);   // newline-joined drained lines; bytes written
using UpFn      = int (*)();                    // 1 up, 0 down
using DeadmanFn = void (*)(const char* line);

inline bool send(const std::string& line) {
    auto f = reinterpret_cast<SendFn>(g_comms_send_fn_);
    return f ? f(line.c_str()) != 0 : false;
}

inline bool up() {
    auto f = reinterpret_cast<UpFn>(g_comms_up_fn_);
    return f ? f() != 0 : false;
}

inline void set_deadman(const std::string& line) {
    auto f = reinterpret_cast<DeadmanFn>(g_comms_deadman_fn_);
    if (f) f(line.c_str());
}

inline std::vector<std::string> poll() {
    std::vector<std::string> out;
    auto f = reinterpret_cast<PollFn>(g_comms_poll_fn_);
    if (!f) return out;
    std::string buf(65536, '\0');
    int n = f(buf.data(), (int)buf.size());
    if (n <= 0) return out;
    buf.resize((size_t)n);
    size_t pos = 0, nl;
    while ((nl = buf.find('\n', pos)) != std::string::npos) {
        if (nl > pos) out.push_back(buf.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (pos < buf.size()) out.push_back(buf.substr(pos));
    return out;
}

}  // namespace comms
}  // namespace xi
