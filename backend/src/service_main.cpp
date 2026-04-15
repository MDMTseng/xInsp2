//
// service_main.cpp — xinsp-backend.exe entry point (M2 skeleton).
//
// Responsibilities in this milestone:
//   - parse --port
//   - start the WS server
//   - handle cmd: ping, version, shutdown
//   - echo anything else as an error rsp
//
// M3 adds run + vars. M4 adds previews. M5 adds compile_and_load.
//

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <xi/xi_protocol.hpp>
#include <xi/xi_ws_server.hpp>

namespace xp = xi::proto;

static std::atomic<bool> g_should_exit{false};

static int parse_port(int argc, char** argv) {
    int port = 7823;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind("--port=", 0) == 0) {
            try { port = std::stoi(std::string(a.substr(7))); } catch (...) {}
        } else if (a == "--port" && i + 1 < argc) {
            try { port = std::stoi(argv[++i]); } catch (...) {}
        }
    }
    return port;
}

static double now_seconds() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

static void send_rsp_ok(xi::ws::Server& srv, int64_t id, std::string data_json = "") {
    xp::Rsp r;
    r.id = id;
    r.ok = true;
    r.data_json = std::move(data_json);
    srv.send_text(r.to_json());
}

static void send_rsp_err(xi::ws::Server& srv, int64_t id, std::string err) {
    xp::Rsp r;
    r.id = id;
    r.ok = false;
    r.error = std::move(err);
    srv.send_text(r.to_json());
}

static void send_hello(xi::ws::Server& srv) {
    xp::Event e;
    e.name = "hello";
    e.data_json = R"({"version":"0.1.0","abi":1})";
    srv.send_text(e.to_json());
}

static void handle_command(xi::ws::Server& srv, std::string_view text) {
    auto parsed = xp::parse_cmd(text);
    if (!parsed) {
        xp::LogMsg lm;
        lm.level = "error";
        lm.msg   = std::string("malformed cmd: ") + std::string(text.substr(0, 128));
        srv.send_text(lm.to_json());
        return;
    }

    const auto& name = parsed->name;
    const int64_t id = parsed->id;

    if (name == "ping") {
        char buf[128];
        std::snprintf(buf, sizeof(buf), R"({"pong":true,"ts":%.3f})", now_seconds());
        send_rsp_ok(srv, id, buf);
    } else if (name == "version") {
        send_rsp_ok(srv, id, R"({"version":"0.1.0","abi":1,"commit":"dev"})");
    } else if (name == "shutdown") {
        send_rsp_ok(srv, id);
        g_should_exit = true;
    } else {
        send_rsp_err(srv, id, std::string("unknown command: ") + name);
    }
}

int main(int argc, char** argv) {
    int port = parse_port(argc, argv);

    xi::ws::Server srv;
    srv.on_open  = [&] {
        std::fprintf(stderr, "[xinsp2] client connected\n");
        send_hello(srv);
    };
    srv.on_close = [&] {
        std::fprintf(stderr, "[xinsp2] client disconnected\n");
    };
    srv.on_text = [&](std::string_view s) {
        handle_command(srv, s);
    };
    srv.on_binary = [&](const uint8_t*, size_t n) {
        std::fprintf(stderr, "[xinsp2] unexpected binary frame: %zu bytes\n", n);
    };

    if (!srv.start(port)) {
        std::fprintf(stderr, "[xinsp2] failed to start on port %d\n", port);
        return 1;
    }
    std::fprintf(stderr, "[xinsp2] listening on ws://127.0.0.1:%d\n", port);
    std::fflush(stderr);

    while (!g_should_exit.load() && srv.is_running()) {
        srv.poll(100);
    }

    srv.stop();
    std::fprintf(stderr, "[xinsp2] shutdown complete\n");
    return 0;
}
