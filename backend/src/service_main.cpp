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

#include <xi/xi.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_jpeg.hpp>
#include <xi/xi_protocol.hpp>
#include <xi/xi_ws_server.hpp>

namespace xp = xi::proto;

// ---- Built-in demo inspect() ----
//
// In M3 the "script" is hardcoded in the service. M5 replaces this with
// a dynamically loaded user .dll. The demo exercises every VarTraits
// specialization plus parallelism.

static int demo_square(int x) {
    return x * x;
}
ASYNC_WRAP(demo_square)

static xi::Param<int>    demo_amp {"demo_amp",  10,  {1, 100}};
static xi::Param<double> demo_bias{"demo_bias", 0.5, {0.0, 1.0}};

// Build a small test image — 3-channel gradient that varies with `frame`
// so successive runs visibly differ in the preview.
static xi::Image demo_make_image(int frame) {
    const int W = 64, H = 48, C = 3;
    xi::Image img(W, H, C);
    uint8_t* p = img.data();
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            p[(y * W + x) * C + 0] = static_cast<uint8_t>((x * 4 + frame) & 0xFF);
            p[(y * W + x) * C + 1] = static_cast<uint8_t>((y * 4 + frame) & 0xFF);
            p[(y * W + x) * C + 2] = static_cast<uint8_t>((x + y + frame) & 0xFF);
        }
    }
    return img;
}

static void demo_inspect(int frame) {
    VAR(gray,    frame * static_cast<int>(demo_amp));
    VAR(doubled, gray * 2);

    auto p1 = async_demo_square(gray);
    auto p2 = async_demo_square(doubled);
    VAR(sq1, int(p1));
    VAR(sq2, int(p2));

    VAR(score, sq1 + sq2 + static_cast<double>(demo_bias));
    VAR(label, std::string("demo"));
    VAR(ok,    true);

    VAR(frame_img, demo_make_image(frame));
}

static std::atomic<int64_t> g_run_id{0};


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
    } else if (name == "run") {
        // Execute the (hardcoded for M3) inspection, collect vars, send back.
        int64_t run_id = ++g_run_id;
        xi::ValueStore::current().clear();
        auto t0 = std::chrono::steady_clock::now();
        try {
            demo_inspect(/*frame=*/7);
        } catch (const std::exception& e) {
            send_rsp_err(srv, id, std::string("inspect threw: ") + e.what());
            return;
        }
        auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0).count();

        // Build vars message from the store snapshot.
        xp::Vars vmsg;
        vmsg.run_id = run_id;
        auto snap = xi::ValueStore::current().snapshot();

        // We'll need to re-walk the image entries after sending the vars
        // message to emit the preview binary frames. Track them by gid.
        struct ImgJob { uint32_t gid; xi::Image img; };
        std::vector<ImgJob> img_jobs;

        uint32_t next_gid = 100;
        for (auto& e : snap) {
            xp::VarItem it;
            it.name = e.name;
            switch (e.kind) {
                case xi::VarKind::Number:
                    it.kind = xp::VarKindWire::Number;
                    it.value_json = e.inline_json;
                    break;
                case xi::VarKind::Boolean:
                    it.kind = xp::VarKindWire::Boolean;
                    it.value_bool = (e.inline_json == "true");
                    break;
                case xi::VarKind::String: {
                    it.kind = xp::VarKindWire::String;
                    // payload holds a std::string
                    if (e.payload.has_value()) {
                        try { it.value_str = std::any_cast<std::string>(e.payload); }
                        catch (...) {}
                    }
                    break;
                }
                case xi::VarKind::Image: {
                    it.kind = xp::VarKindWire::Image;
                    it.gid  = next_gid++;
                    it.raw  = false;
                    if (e.payload.has_value()) {
                        try {
                            auto img = std::any_cast<xi::Image>(e.payload);
                            img_jobs.push_back({it.gid, std::move(img)});
                        } catch (...) {}
                    }
                    break;
                }
                case xi::VarKind::Json:
                    it.kind = xp::VarKindWire::Json;
                    it.value_json = e.inline_json.empty() ? "null" : e.inline_json;
                    break;
                default:
                    it.kind = xp::VarKindWire::Custom;
                    it.value_json = "null";
            }
            vmsg.items.push_back(std::move(it));
        }

        // Respond to the `run` command first with timing, then emit vars,
        // then one binary preview frame per image variable.
        char buf[128];
        std::snprintf(buf, sizeof(buf), R"({"run_id":%lld,"ms":%lld})",
                     (long long)run_id, (long long)dt_ms);
        send_rsp_ok(srv, id, buf);
        srv.send_text(vmsg.to_json());

        for (auto& job : img_jobs) {
            std::vector<uint8_t> jpeg;
            if (!xi::encode_jpeg(job.img, 85, jpeg)) continue;
            std::vector<uint8_t> frame;
            frame.resize(xp::kPreviewHeaderSize + jpeg.size());
            xp::PreviewHeader h;
            h.gid      = job.gid;
            h.codec    = static_cast<uint32_t>(xp::Codec::JPEG);
            h.width    = static_cast<uint32_t>(job.img.width);
            h.height   = static_cast<uint32_t>(job.img.height);
            h.channels = static_cast<uint32_t>(job.img.channels);
            xp::encode_preview_header(h, frame.data());
            std::memcpy(frame.data() + xp::kPreviewHeaderSize,
                        jpeg.data(), jpeg.size());
            srv.send_binary(frame.data(), frame.size());
        }
    } else if (name == "list_params") {
        // Emit a minimal instances message with just the param registry.
        std::string out = "{\"type\":\"instances\",\"instances\":[],\"params\":[";
        auto list = xi::ParamRegistry::instance().list();
        for (size_t i = 0; i < list.size(); ++i) {
            if (i) out += ",";
            out += list[i]->as_json();
        }
        out += "]}";
        send_rsp_ok(srv, id, "{}");
        srv.send_text(out);
    } else if (name == "set_param") {
        auto pname = xp::get_string_field(parsed->args_json, "name");
        if (!pname) {
            send_rsp_err(srv, id, "set_param: missing name");
            return;
        }
        auto* p = xi::ParamRegistry::instance().find(*pname);
        if (!p) {
            send_rsp_err(srv, id, std::string("no such param: ") + *pname);
            return;
        }
        // Extract raw value substring from args_json. get_number_field
        // handles int/float; for bool we fall back to a string check.
        auto num = xp::get_number_field(parsed->args_json, "value");
        bool ok = false;
        if (num) {
            char nb[64];
            std::snprintf(nb, sizeof(nb), "%g", *num);
            ok = p->set_from_json(nb);
        } else {
            // Maybe "value":true / "value":false
            auto sv = xp::get_string_field(parsed->args_json, "value");
            if (sv) ok = p->set_from_json(*sv);
            else {
                if (parsed->args_json.find("\"value\":true")  != std::string::npos) ok = p->set_from_json("true");
                if (parsed->args_json.find("\"value\":false") != std::string::npos) ok = p->set_from_json("false");
            }
        }
        if (ok) send_rsp_ok(srv, id);
        else    send_rsp_err(srv, id, "set_param: bad value");
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
