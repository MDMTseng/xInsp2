#pragma once
//
// xi_comms_gateway.hpp — GatewayClient: the backend's client to the
// out-of-process comms gateway (xinsp-comms). Connects over loopback and backs
// the script's xi::comms::* API. The gateway owns the PLC link; this class just
// relays newline-JSON ops and buffers PLC-originated lines for poll(). A
// background reader thread keeps the inbox + link state current.
//
// Extracted from service_main.cpp: it's a self-contained subsystem (Winsock +
// cJSON + xi::proto::json_escape_into only — no service-main globals), so it lives in
// its own leaf header. The g_gateway pointer + the xi::comms callbacks that
// reference it stay in service_main. See docs/design/comms-gateway.md.
//

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#endif

#include <cJSON.h>
#include <xi/xi_protocol.hpp>   // xi::proto::json_escape_into

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace xi {

class GatewayClient {
public:
    bool connect(int port) {
        // Winsock may not be up yet (we connect before srv.start()); WSAStartup
        // is refcounted, so an extra call here is safe.
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
        for (int attempt = 0; attempt < 15; ++attempt) {   // tolerate FE spawn race
            SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s != INVALID_SOCKET) {
                sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
                InetPtonA(AF_INET, "127.0.0.1", &a.sin_addr);
                if (::connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
                    sock_ = s; run_ = true;
                    reader_ = std::thread([this] { reader_loop(); });
                    return true;
                }
                closesocket(s);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    }
    void stop() {
        run_ = false;
        if (sock_ != INVALID_SOCKET) { shutdown(sock_, 2); closesocket(sock_); sock_ = INVALID_SOCKET; }
        if (reader_.joinable()) reader_.join();
    }
    bool send_line(const std::string& line) {   // op:send
        std::string m = "{\"op\":\"send\",\"line\":";
        xi::proto::json_escape_into(m, line); m += "}";
        return write_(m);
    }
    void set_deadman(const std::string& line) {
        std::string m = "{\"op\":\"set_deadman\",\"line\":";
        xi::proto::json_escape_into(m, line); m += "}";
        write_(m);
    }
    void say_bye() { write_("{\"op\":\"bye\"}"); }
    bool up() const { return up_.load(std::memory_order_relaxed); }
    // Drain buffered PLC-originated lines, newline-joined, into buf; return bytes.
    int drain(char* buf, int buflen) {
        std::lock_guard<std::mutex> lk(in_mu_);
        int used = 0;
        while (!inbox_.empty()) {
            const std::string& l = inbox_.front();
            int need = (int)l.size() + 1;
            if (used + need > buflen) break;
            std::memcpy(buf + used, l.data(), l.size());
            used += (int)l.size();
            buf[used++] = '\n';
            inbox_.pop_front();
        }
        return used;
    }
private:
    bool write_(const std::string& msg) {
        if (sock_ == INVALID_SOCKET) return false;
        std::string out = msg; out.push_back('\n');
        std::lock_guard<std::mutex> lk(send_mu_);
        return ::send(sock_, out.data(), (int)out.size(), 0) == (int)out.size();
    }
    void reader_loop() {
        std::string buf;
        char tmp[4096];
        while (run_.load()) {
            int r = ::recv(sock_, tmp, sizeof(tmp), 0);
            if (r <= 0) { up_ = false; break; }
            buf.append(tmp, r);
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos); buf.erase(0, pos + 1);
                if (line.empty()) continue;
                cJSON* root = cJSON_Parse(line.c_str());
                if (!root) continue;
                cJSON* ev = cJSON_GetObjectItem(root, "event");
                if (cJSON_IsString(ev)) {
                    if (std::strcmp(ev->valuestring, "plc_in") == 0) {
                        if (cJSON* l = cJSON_GetObjectItem(root, "line"); cJSON_IsString(l)) {
                            std::lock_guard<std::mutex> lk(in_mu_);
                            if (inbox_.size() < 4096) inbox_.emplace_back(l->valuestring);
                        }
                    } else if (std::strcmp(ev->valuestring, "plc_up") == 0) {
                        cJSON* u = cJSON_GetObjectItem(root, "up");
                        up_ = cJSON_IsTrue(u);
                    }
                }
                cJSON_Delete(root);
            }
        }
    }
    SOCKET                   sock_ = INVALID_SOCKET;
    std::thread              reader_;
    std::atomic<bool>        run_{false};
    std::atomic<bool>        up_{false};
    std::mutex               in_mu_;
    std::deque<std::string>  inbox_;
    std::mutex               send_mu_;
};

} // namespace xi
