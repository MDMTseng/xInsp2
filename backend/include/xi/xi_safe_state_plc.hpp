#pragma once
//
// xi_safe_state_plc.hpp — a SafeStateSink that commands a PLC over TCP or UDP
// with a newline-terminated JSON message. Selected via the FE's --safe-state
// flag: "tcp:HOST:PORT" or "udp:HOST:PORT" (anything else falls back to the
// logging stub in xi_safe_state.hpp).
//
// Win32/Winsock (the FE already WSAStartup's + links Ws2_32). TODO(linux): the
// same with BSD sockets — the message-building + sink contract are portable; only
// the socket calls are gated.
//
// Reliability (this is a SAFETY path):
//   - UDP: best-effort, so the "enter safe state" datagram is sent N times to
//     survive a dropped packet. (clear is sent once.)
//   - TCP: per-message connect (non-blocking, short timeout) + send + close —
//     robust to a PLC that restarts between messages, no stale socket to manage.
//   - Never throws; transport failures are logged, not propagated (going safe is
//     the last thing we can do for the line — we must not crash trying).
//
// MESSAGE SCHEMA (newline-delimited JSON) — adapt build_*() to your PLC's keys:
//   enter: {"src":"xinsp-fe","event":"safe_state","state":"enter",
//           "reason":"BackendExit","rc":"0xC0000005","module":"plugin_v0.dll",
//           "phase":"inspect","ts_ms":1780...}
//   clear: {"src":"xinsp-fe","event":"safe_state","state":"clear","ts_ms":1780...}
//
#include <xi/xi_safe_state.hpp>
#include <xi/xi_clock.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

namespace xi {

namespace detail {
inline void plc_json_escape(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                else out.push_back(c);
        }
    }
    out.push_back('"');
}
inline int64_t plc_now_ms() { return xi::wall_ms(); }   // wall: PLC event ts
}  // namespace detail

class PlcSafeStateSink : public SafeStateSink {
public:
    enum class Proto { Tcp, Udp };

    PlcSafeStateSink(Proto proto, std::string host, int port, std::FILE* log = nullptr)
        : proto_(proto), host_(std::move(host)), port_(port), log_(log) {}

    void enter_safe_state(const SafeStateEvent& ev) override {
        std::string js = build_enter(ev);
        log_line("ENTER SAFE STATE", js);
        // Safety-critical: repeat on UDP to survive a dropped datagram.
        send_msg(js, proto_ == Proto::Udp ? 3 : 1);
    }

    void clear_safe_state() override {
        std::string js = build_clear();
        log_line("CLEAR SAFE STATE", js);
        send_msg(js, 1);
    }

    const char* name() const override { return proto_ == Proto::Tcp ? "plc-tcp" : "plc-udp"; }

private:
    std::string endpoint() const {
        return (proto_ == Proto::Tcp ? "tcp:" : "udp:") + host_ + ":" + std::to_string(port_);
    }

    std::string build_enter(const SafeStateEvent& ev) const {
        std::string s = "{\"src\":\"xinsp-fe\",\"event\":\"safe_state\",\"state\":\"enter\",\"reason\":";
        detail::plc_json_escape(s, to_string(ev.reason));
        char rc[24]; std::snprintf(rc, sizeof(rc), "\"0x%08X\"", (unsigned)ev.backend_rc);
        s += ",\"rc\":"; s += rc;
        s += ",\"module\":"; detail::plc_json_escape(s, ev.faulting_module);
        s += ",\"phase\":";  detail::plc_json_escape(s, ev.last_phase);
        if (!ev.custom_payload.empty()) {
            s += ",\"payload\":"; detail::plc_json_escape(s, ev.custom_payload);
        }
        s += ",\"ts_ms\":" + std::to_string(ev.ts_ms ? ev.ts_ms : detail::plc_now_ms());
        s += "}\n";
        return s;
    }

    std::string build_clear() const {
        return "{\"src\":\"xinsp-fe\",\"event\":\"safe_state\",\"state\":\"clear\",\"ts_ms\":"
             + std::to_string(detail::plc_now_ms()) + "}\n";
    }

    void log_line(const char* what, const std::string& js) {
        std::string line = js;
        if (!line.empty() && line.back() == '\n') line.pop_back();
        std::fprintf(stderr, "[xinsp-fe] %s -> PLC %s %s\n", what, endpoint().c_str(), line.c_str());
        std::fflush(stderr);
        if (log_) { std::fprintf(log_, "[xinsp-fe] %s -> PLC %s %s\n", what, endpoint().c_str(), line.c_str()); std::fflush(log_); }
    }

    // Resolve host:port into a sockaddr_in (numeric or hostname).
    bool resolve(sockaddr_in& addr) const {
        addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)port_);
        if (InetPtonA(AF_INET, host_.c_str(), &addr.sin_addr) == 1) return true;
        // Fall back to DNS for a hostname.
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype =
            (proto_ == Proto::Tcp ? SOCK_STREAM : SOCK_DGRAM);
        addrinfo* res = nullptr;
        if (getaddrinfo(host_.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
        addr.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
        return true;
    }

    void send_msg(const std::string& payload, int repeats) {
        sockaddr_in addr;
        if (!resolve(addr)) { warn("resolve failed"); return; }
        if (proto_ == Proto::Udp) {
            SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET) { warn("socket"); return; }
            for (int i = 0; i < repeats; ++i)
                ::sendto(s, payload.data(), (int)payload.size(), 0, (sockaddr*)&addr, sizeof(addr));
            closesocket(s);
        } else {
            SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) { warn("socket"); return; }
            // Non-blocking connect with a short timeout so a down PLC can't stall
            // the FE monitor thread.
            u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
            bool connected = false;
            if (::connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
                connected = true;
            } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
                timeval tv{0, 500 * 1000};  // 500 ms
                connected = ::select(0, nullptr, &wf, nullptr, &tv) > 0;
            }
            if (connected) {
                u_long bl = 0; ioctlsocket(s, FIONBIO, &bl);  // blocking send
                ::send(s, payload.data(), (int)payload.size(), 0);
            } else {
                warn("connect timed out/failed");
            }
            closesocket(s);
        }
    }

    void warn(const char* why) {
        std::fprintf(stderr, "[xinsp-fe] WARNING: PLC %s send failed (%s)\n", endpoint().c_str(), why);
        std::fflush(stderr);
    }

    Proto       proto_;
    std::string host_;
    int         port_;
    std::FILE*  log_;
};

// Parse a "tcp:HOST:PORT" / "udp:HOST:PORT" spec into a PLC sink. Returns nullptr
// if `spec` is not a net spec (caller then falls back to the logging sink).
inline std::unique_ptr<SafeStateSink> make_plc_sink(const std::string& spec, std::FILE* log = nullptr) {
    PlcSafeStateSink::Proto proto;
    if      (spec.rfind("tcp:", 0) == 0) proto = PlcSafeStateSink::Proto::Tcp;
    else if (spec.rfind("udp:", 0) == 0) proto = PlcSafeStateSink::Proto::Udp;
    else return nullptr;
    std::string rest = spec.substr(4);
    auto colon = rest.rfind(':');
    if (colon == std::string::npos) return nullptr;
    std::string host = rest.substr(0, colon);
    int port = 0;
    try { port = std::stoi(rest.substr(colon + 1)); } catch (...) { return nullptr; }
    if (host.empty() || port <= 0 || port > 65535) return nullptr;
    return std::make_unique<PlcSafeStateSink>(proto, host, port, log);
}

}  // namespace xi
#endif  // _WIN32
