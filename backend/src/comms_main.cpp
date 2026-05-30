//
// comms_main.cpp — xinsp-comms.exe, the out-of-process comms gateway.
//
// Owns the (volatile) PLC connection and relays newline-delimited messages
// between ONE loopback client (the backend) and the PLC over TCP or UDP. See
// docs/design/comms-gateway.md.
//
// Deliberately SCHEMA-AGNOSTIC: it never parses the PLC's payload — it relays
// opaque "line" strings. Only the small control envelope between client and
// gateway is JSON, so the PLC wire (JSON, msgpack-as-text, whatever) can evolve
// without touching the gateway.
//
//   client -> gateway : {"id":N,"op":"send","line":"<verbatim to PLC>"}
//                       {"id":N,"op":"set_deadman","line":"<emergency line to PLC>"}
//                       {"id":N,"op":"bye"}                               (clean shutdown)
//                       {"id":N,"op":"ping"}
//   gateway -> client : {"id":N,"ok":true}  /  {"id":N,"ok":false,"err":"..."}
//                       {"event":"plc_in","line":"<verbatim from PLC>"}   (async)
//                       {"event":"plc_up","up":true|false}                (link state)
//
// DEAD-MAN SAFETY: the gateway owns the PLC link, so it is the thing that tells
// the PLC to go safe when the backend dies. The backend registers an emergency
// payload via "set_deadman"; if the backend's connection drops WITHOUT a "bye"
// (i.e. it crashed), the gateway immediately sends that payload to the PLC for
// emergency handling. (And if the gateway itself dies, the PLC sees its own
// connection drop and can dead-man on its own.) The schema of the emergency
// payload stays the backend's — the gateway just relays the registered line.
//
// Isolation is the point: if this process crashes/hangs it takes only itself
// down. Win32/Winsock; TODO(linux).
//
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <cJSON.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "Ws2_32.lib")
#endif

static std::string arg_value(int argc, char** argv, const char* flag) {
    std::string eq = std::string(flag) + "=";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind(eq, 0) == 0) return a.substr(eq.size());
        if (a == flag && i + 1 < argc) return argv[i + 1];
    }
    return {};
}
static bool has_flag(int argc, char** argv, const char* f) {
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == f) return true;
    return false;
}

static void json_escape(std::string& out, const std::string& s) {
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

#ifdef _WIN32
// Accumulates bytes and yields complete '\n'-delimited lines.
struct LineReader {
    std::string buf;
    template <class Fn>
    void feed(const char* data, int n, Fn&& on_line) {
        buf.append(data, n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buf.erase(0, pos + 1);
            if (!line.empty()) on_line(line);
        }
    }
};

static void send_line(SOCKET s, const std::string& line) {
    if (s == INVALID_SOCKET) return;
    std::string out = line; out.push_back('\n');
    ::send(s, out.data(), (int)out.size(), 0);
}

struct PlcEndpoint {
    bool        udp = false;
    std::string host;
    int         port = 0;
    SOCKET      sock = INVALID_SOCKET;
    sockaddr_in addr{};   // for UDP sendto
    bool        up = false;
};

static bool resolve_addr(const std::string& host, int port, bool udp, sockaddr_in& addr) {
    addr = {}; addr.sin_family = AF_INET; addr.sin_port = htons((u_short)port);
    if (InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) == 1) return true;
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    addr.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

// (Re)connect the PLC link. UDP "connects" only in the sense of a bound socket +
// fixed peer (connectionless). Returns true if the socket is ready.
static bool plc_open(PlcEndpoint& p) {
    if (p.sock != INVALID_SOCKET) { closesocket(p.sock); p.sock = INVALID_SOCKET; }
    if (!resolve_addr(p.host, p.port, p.udp, p.addr)) return false;
    if (p.udp) {
        p.sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (p.sock == INVALID_SOCKET) return false;
        // bound implicitly on first send; recvfrom works after.
        p.up = true;
        return true;
    }
    p.sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (p.sock == INVALID_SOCKET) return false;
    u_long nb = 1; ioctlsocket(p.sock, FIONBIO, &nb);
    bool ok = false;
    if (::connect(p.sock, (sockaddr*)&p.addr, sizeof(p.addr)) == 0) ok = true;
    else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wf; FD_ZERO(&wf); FD_SET(p.sock, &wf);
        timeval tv{0, 500 * 1000};
        ok = ::select(0, nullptr, &wf, nullptr, &tv) > 0;
    }
    u_long bl = 0; ioctlsocket(p.sock, FIONBIO, &bl);
    if (!ok) { closesocket(p.sock); p.sock = INVALID_SOCKET; return false; }
    p.up = true;
    return true;
}

static void plc_send(PlcEndpoint& p, const std::string& line) {
    std::string out = line; out.push_back('\n');
    if (p.udp) ::sendto(p.sock, out.data(), (int)out.size(), 0, (sockaddr*)&p.addr, sizeof(p.addr));
    else       ::send(p.sock, out.data(), (int)out.size(), 0);
}

int main(int argc, char** argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        std::printf(
            "xinsp-comms - xInsp2 comms gateway (out-of-process PLC I/O)\n\n"
            "Relays newline-JSON between one loopback client (the backend) and a PLC.\n"
            "Usage: xinsp-comms --plc=tcp:HOST:PORT|udp:HOST:PORT [--listen=PORT]\n"
            "  --plc=SPEC      PLC endpoint (tcp: or udp:)\n"
            "  --listen=PORT   loopback port for the backend client (default 7900)\n"
            "  --help, -h      this help\n");
        return 0;
    }

    std::string plc_spec = arg_value(argc, argv, "--plc");
    if (plc_spec.rfind("tcp:", 0) != 0 && plc_spec.rfind("udp:", 0) != 0) {
        std::fprintf(stderr, "[xinsp-comms] --plc=tcp:HOST:PORT or udp:HOST:PORT required\n");
        return 2;
    }
    PlcEndpoint plc;
    plc.udp = plc_spec.rfind("udp:", 0) == 0;
    { std::string rest = plc_spec.substr(4); auto c = rest.rfind(':');
      if (c == std::string::npos) { std::fprintf(stderr, "[xinsp-comms] bad --plc\n"); return 2; }
      plc.host = rest.substr(0, c);
      try { plc.port = std::stoi(rest.substr(c + 1)); } catch (...) { return 2; } }
    int listen_port = 7900;
    if (auto v = arg_value(argc, argv, "--listen"); !v.empty()) try { listen_port = std::stoi(v); } catch (...) {}

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET lsock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int yes = 1; ::setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in la{}; la.sin_family = AF_INET; la.sin_port = htons((u_short)listen_port);
    InetPtonA(AF_INET, "127.0.0.1", &la.sin_addr);
    if (::bind(lsock, (sockaddr*)&la, sizeof(la)) != 0 || ::listen(lsock, 4) != 0) {
        std::fprintf(stderr, "[xinsp-comms] failed to listen on 127.0.0.1:%d\n", listen_port);
        return 1;
    }
    std::fprintf(stderr, "[xinsp-comms] plc=%s listen=127.0.0.1:%d\n", plc_spec.c_str(), listen_port);
    std::fflush(stderr);

    plc_open(plc);   // best-effort; retried in the loop if down
    std::fprintf(stderr, "[xinsp-comms] plc %s\n", plc.up ? "connected" : "down (will retry)");

    SOCKET client = INVALID_SOCKET;
    LineReader client_lr, plc_lr;
    std::string deadman;          // emergency line to send to the PLC if the
    bool        client_said_bye = false;   // backend drops without a clean "bye"

    for (;;) {
        fd_set rfds; FD_ZERO(&rfds);
        FD_SET(lsock, &rfds);
        SOCKET maxfd = lsock;
        if (client != INVALID_SOCKET) { FD_SET(client, &rfds); maxfd = (std::max)(maxfd, client); }
        if (plc.up && plc.sock != INVALID_SOCKET) { FD_SET(plc.sock, &rfds); maxfd = (std::max)(maxfd, plc.sock); }
        timeval tv{1, 0};
        int n = ::select((int)maxfd + 1, &rfds, nullptr, nullptr, &tv);

        // Retry a down TCP PLC link ~every second.
        if (!plc.up && !plc.udp) {
            if (plc_open(plc) && client != INVALID_SOCKET)
                send_line(client, "{\"event\":\"plc_up\",\"up\":true}");
        }
        if (n <= 0) continue;

        if (FD_ISSET(lsock, &rfds)) {
            SOCKET s = ::accept(lsock, nullptr, nullptr);
            if (s != INVALID_SOCKET) {
                if (client != INVALID_SOCKET) closesocket(client);  // single client
                client = s; client_lr.buf.clear();
                deadman.clear(); client_said_bye = false;   // each backend re-arms its own
                std::fprintf(stderr, "[xinsp-comms] client connected\n");
                send_line(client, plc.up ? "{\"event\":\"plc_up\",\"up\":true}"
                                         : "{\"event\":\"plc_up\",\"up\":false}");
            }
        }

        if (client != INVALID_SOCKET && FD_ISSET(client, &rfds)) {
            char buf[4096];
            int r = ::recv(client, buf, sizeof(buf), 0);
            if (r <= 0) {
                // Backend gone. If it didn't say "bye" it CRASHED — fire the
                // registered emergency payload to the PLC so it can go safe.
                if (!client_said_bye && !deadman.empty() && plc.up) {
                    std::fprintf(stderr, "[xinsp-comms] backend lost without bye -> "
                                 "sending dead-man payload to PLC\n");
                    plc_send(plc, deadman);
                } else {
                    std::fprintf(stderr, "[xinsp-comms] client disconnected%s\n",
                                 client_said_bye ? " (clean)" : "");
                }
                closesocket(client); client = INVALID_SOCKET;
                deadman.clear(); client_said_bye = false;
            } else {
                client_lr.feed(buf, r, [&](const std::string& line) {
                    cJSON* root = cJSON_Parse(line.c_str());
                    long long id = 0; std::string op, payload;
                    bool have_id = false;
                    if (root) {
                        if (cJSON* j = cJSON_GetObjectItem(root, "id"); cJSON_IsNumber(j)) { id = (long long)j->valuedouble; have_id = true; }
                        if (cJSON* j = cJSON_GetObjectItem(root, "op"); cJSON_IsString(j)) op = j->valuestring;
                        if (cJSON* j = cJSON_GetObjectItem(root, "line"); cJSON_IsString(j)) payload = j->valuestring;
                    }
                    auto reply = [&](bool ok, const char* err) {
                        if (!have_id) return;
                        std::string m = "{\"id\":" + std::to_string(id) + ",\"ok\":" + (ok ? "true" : "false");
                        if (!ok && err) { m += ",\"err\":"; json_escape(m, err); }
                        m += "}";
                        send_line(client, m);
                    };
                    if (op == "send") {
                        if (!plc.up) reply(false, "plc link down");
                        else { plc_send(plc, payload); reply(true, nullptr); }
                    } else if (op == "set_deadman") {
                        deadman = payload;          // armed; fired if the client drops w/o bye
                        reply(true, nullptr);
                    } else if (op == "bye") {
                        client_said_bye = true;     // clean shutdown — don't fire the dead-man
                        reply(true, nullptr);
                    } else if (op == "ping") {
                        reply(true, nullptr);
                    } else {
                        reply(false, "unknown op");
                    }
                    if (root) cJSON_Delete(root);
                });
            }
        }

        if (plc.up && plc.sock != INVALID_SOCKET && FD_ISSET(plc.sock, &rfds)) {
            char buf[4096];
            int r;
            if (plc.udp) { sockaddr_in from{}; int fl = sizeof(from);
                           r = ::recvfrom(plc.sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fl); }
            else         { r = ::recv(plc.sock, buf, sizeof(buf), 0); }
            if (r <= 0 && !plc.udp) {
                std::fprintf(stderr, "[xinsp-comms] plc link dropped\n");
                closesocket(plc.sock); plc.sock = INVALID_SOCKET; plc.up = false;
                if (client != INVALID_SOCKET) send_line(client, "{\"event\":\"plc_up\",\"up\":false}");
            } else if (r > 0) {
                plc_lr.feed(buf, r, [&](const std::string& line) {
                    if (client != INVALID_SOCKET) {
                        std::string m = "{\"event\":\"plc_in\",\"line\":";
                        json_escape(m, line);
                        m += "}";
                        send_line(client, m);
                    }
                });
            }
        }
    }
}
#else
int main(int, char**) {
    std::fprintf(stderr, "[xinsp-comms] not implemented on this platform yet\n");
    return 3;  // TODO(linux): BSD sockets
}
#endif
