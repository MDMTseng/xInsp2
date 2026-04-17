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

#include <cJSON.h>
#include <xi/xi.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_jpeg.hpp>
#include <xi/xi_protocol.hpp>
#include <xi/xi_plugin_manager.hpp>
#include <xi/xi_project.hpp>
#include <xi/xi_script_compiler.hpp>
#include <xi/xi_script_loader.hpp>
#include <xi/xi_source.hpp>
#include <xi/xi_ws_server.hpp>

#include <condition_variable>
#include <filesystem>
#include <thread>

namespace xp = xi::proto;

static std::atomic<int64_t> g_run_id{0};

// Loaded user script state. When null, cmd:run returns an error.
static xi::script::LoadedScript g_script;
static std::mutex               g_script_mu;

// Persistent cross-frame state — survives DLL reloads.
static std::string g_persistent_state_json = "{}";

// --- xi::use() callback implementations ---
// These are called FROM the script DLL back INTO the backend, routing
// process/exchange/grab to the backend's InstanceRegistry.

#include <xi/xi_use.hpp>
#include <eh.h>

class seh_exception : public std::exception {
public:
    unsigned int code;
    seh_exception(unsigned int c) : code(c) {}
    const char* what() const noexcept override {
        switch (code) {
            case 0xC0000005: return "ACCESS_VIOLATION";
            case 0xC0000094: return "INT_DIVIDE_BY_ZERO";
            case 0xC000008C: return "ARRAY_BOUNDS_EXCEEDED";
            case 0xC00000FD: return "STACK_OVERFLOW";
            case 0xC000001D: return "ILLEGAL_INSTRUCTION";
            case 0xC0000090: return "FLOAT_INVALID_OPERATION";
            case 0xC0000091: return "FLOAT_DIVIDE_BY_ZERO";
            default:         return "UNKNOWN_SEH_EXCEPTION";
        }
    }
};

static void seh_translator(unsigned int code, EXCEPTION_POINTERS*) {
    throw seh_exception(code);
}

static int use_process_cb(const char* name,
                          const char* input_json,
                          const xi_record_image* input_images, int input_image_count,
                          xi_record_out* output) {
    auto inst = xi::InstanceRegistry::instance().find(name);
    if (!inst) return -1;

    // Check if it's a C ABI adapter with process_fn
    auto* adapter = dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get());
    if (adapter && adapter->process_fn()) {
        xi_record in_rec;
        in_rec.images = input_images;
        in_rec.image_count = input_image_count;
        in_rec.json = input_json;
        try {
            adapter->process_fn()(adapter->raw_instance(), &in_rec, output);
        } catch (const seh_exception& e) {
            std::fprintf(stderr, "[xinsp2] use_process('%s') crashed: 0x%08X (%s)\n",
                         name, e.code, e.what());
            return -2;
        } catch (...) {
            std::fprintf(stderr, "[xinsp2] use_process('%s') threw exception\n", name);
            return -2;
        }
        return output->image_count;
    }
    return -1;
}

static int use_exchange_cb(const char* name, const char* cmd,
                           char* rsp, int rsplen) {
    try {
        auto inst = xi::InstanceRegistry::instance().find(name);
        if (!inst) return -1;
        std::string result = inst->exchange(cmd);
        int n = (int)result.size();
        if (rsplen < n + 1) return -n;
        std::memcpy(rsp, result.data(), result.size());
        rsp[result.size()] = 0;
        return n;
    } catch (const seh_exception& e) {
        std::fprintf(stderr, "[xinsp2] use_exchange('%s') crashed: 0x%08X (%s)\n",
                     name, e.code, e.what());
        return -1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[xinsp2] use_exchange('%s') threw: %s\n", name, e.what());
        return -1;
    }
}

static xi_image_handle use_grab_cb(const char* name, int timeout_ms) {
    auto inst = xi::InstanceRegistry::instance().find(name);
    auto* src = inst ? dynamic_cast<xi::ImageSource*>(inst.get()) : nullptr;
    if (!src) return XI_IMAGE_NULL;
    xi::Image img = src->grab_wait(timeout_ms);
    if (img.empty()) return XI_IMAGE_NULL;
    return xi::ImagePool::instance().from_image(img);
}

// ---- Trigger loop state ----
// When running in continuous mode (cmd: start), a worker thread waits for
// trigger signals from image sources and calls inspect() for each frame.
static std::atomic<bool>       g_continuous{false};
static std::condition_variable g_trigger_cv;
static std::mutex              g_trigger_mu;
static std::atomic<int>        g_trigger_pending{0};
static std::thread             g_worker_thread;

// Forward-declare: runs one inspection cycle and emits vars+previews.
// If run_id == 0, auto-generates one. frame_hint is passed to inspect().
static void run_one_inspection(xi::ws::Server& srv, int frame_hint = 1, int64_t run_id = 0);

// Path resolution for the script compiler. Backend derives its own dir at
// startup and uses that to locate the xi headers we ship alongside the exe.
static std::string g_include_dir;
static std::string g_work_dir;
static std::string g_plugins_dir;

// Plugin manager (global)
static xi::PluginManager g_plugin_mgr;

static std::string get_exe_dir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::filesystem::path p(std::string(buf, n));
    return p.parent_path().string();
}


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

// Shared function: emit vars + binary preview frames from the script's
// thunks or the built-in demo. Called by `cmd: run` and by the continuous
// trigger loop.
static void emit_vars_and_previews(xi::ws::Server& srv,
                                    xi::script::LoadedScript& s,
                                    int64_t run_id, int64_t dt_ms) {
    if (s.ok() && s.snapshot) {
        // Script path — read from DLL thunks
        std::vector<char> sbuf(256 * 1024);
        int n = s.snapshot(sbuf.data(), (int)sbuf.size());
        if (n < 0) { sbuf.resize((size_t)(-n) + 1024);
                     n = s.snapshot(sbuf.data(), (int)sbuf.size()); }
        if (n <= 0) return;

        // vars text message
        std::string vars_msg = "{\"type\":\"vars\",\"run_id\":";
        vars_msg += std::to_string(run_id);
        vars_msg += ",\"items\":";
        vars_msg += std::string(sbuf.data(), (size_t)n);
        vars_msg += "}";
        srv.send_text(vars_msg);

        // image previews
        std::string_view snap_view(sbuf.data(), (size_t)n);
        size_t pos = 0;
        while (true) {
            pos = snap_view.find("\"gid\":", pos);
            if (pos == std::string_view::npos) break;
            pos += 6;
            uint32_t gid = (uint32_t)std::atoi(snap_view.data() + pos);
            if (s.dump_image) {
                int w = 0, h = 0, c = 0;
                std::vector<uint8_t> raw(32 * 1024 * 1024);
                int nb = s.dump_image(gid, raw.data(), (int)raw.size(), &w, &h, &c);
                if (nb > 0 && w > 0 && h > 0 && c > 0) {
                    xi::Image img(w, h, c, raw.data());
                    std::vector<uint8_t> jpeg;
                    if (xi::encode_jpeg(img, 85, jpeg)) {
                        std::vector<uint8_t> frame(xp::kPreviewHeaderSize + jpeg.size());
                        xp::PreviewHeader hd;
                        hd.gid = gid; hd.codec = (uint32_t)xp::Codec::JPEG;
                        hd.width = (uint32_t)w; hd.height = (uint32_t)h; hd.channels = (uint32_t)c;
                        xp::encode_preview_header(hd, frame.data());
                        std::memcpy(frame.data() + xp::kPreviewHeaderSize, jpeg.data(), jpeg.size());
                        srv.send_binary(frame.data(), frame.size());
                    }
                }
            }
        }
    }
}

// (seh_exception and seh_translator defined above, before use_process_cb)

// Run one full inspection cycle: reset → inspect → emit.
// The inspect call is wrapped in SEH so a script crash (null deref,
// divide-by-zero, stack overflow) is caught without killing the backend.
static void run_one_inspection(xi::ws::Server& srv, int frame_hint, int64_t run_id) {
    if (run_id == 0) run_id = ++g_run_id;

    xi::script::LoadedScript s;
    {
        std::lock_guard<std::mutex> lk(g_script_mu);
        s = g_script;
    }

    if (!s.ok()) {
        xp::LogMsg lm;
        lm.level = "warn";
        lm.msg = "no script loaded — compile a .cpp first";
        srv.send_text(lm.to_json());
        return;
    }

    auto t0 = std::chrono::steady_clock::now();
    try {
        if (s.reset) s.reset();
        s.inspect(frame_hint);
    } catch (const seh_exception& e) {
        auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0).count();
        char msg[256];
        std::snprintf(msg, sizeof(msg), "script crashed after %lldms: 0x%08X (%s)",
                     (long long)dt_ms, e.code, e.what());
        std::fprintf(stderr, "[xinsp2] %s\n", msg);
        xp::LogMsg lm; lm.level = "error"; lm.msg = msg;
        srv.send_text(lm.to_json());
        return;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[xinsp2] inspect threw: %s\n", e.what());
        xp::LogMsg lm; lm.level = "error"; lm.msg = std::string("script exception: ") + e.what();
        srv.send_text(lm.to_json());
        return;
    }

    auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0).count();
    emit_vars_and_previews(srv, s, run_id, dt_ms);
}

// (trigger_worker removed — continuous mode uses a simple timer thread)

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
        // Stop continuous mode first to avoid use-after-free on srv
        if (g_continuous.load()) {
            g_continuous = false;
            if (g_worker_thread.joinable()) g_worker_thread.join();
        }
        send_rsp_ok(srv, id);
        g_should_exit = true;
    } else if (name == "compile_and_load") {
        auto src = xp::get_string_field(parsed->args_json, "path");
        if (!src) {
            send_rsp_err(srv, id, "compile_and_load: missing path");
            return;
        }

        // Stop continuous mode before reloading — the worker thread holds
        // function pointers into the DLL we're about to unload.
        if (g_continuous.load()) {
            g_continuous = false;
            if (g_worker_thread.joinable()) g_worker_thread.join();
            std::fprintf(stderr, "[xinsp2] stopped continuous mode for reload\n");
        }

        xi::script::CompileRequest req;
        req.source_path = *src;
        req.output_dir  = (std::filesystem::path(g_work_dir) / "script_build").string();
        req.include_dir = g_include_dir;

        auto res = xi::script::compile(req);
        if (!res.ok) {
            xp::Rsp r;
            r.id = id;
            r.ok = false;
            r.error = "compile failed";
            srv.send_text(r.to_json());
            xp::LogMsg lm;
            lm.level = "error";
            lm.msg = res.build_log;
            srv.send_text(lm.to_json());
            return;
        }

        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            // Save persistent state before unloading old DLL
            if (g_script.ok() && g_script.get_state) {
                std::vector<char> buf(256 * 1024);
                int n = g_script.get_state(buf.data(), (int)buf.size());
                if (n < 0) { buf.resize((size_t)(-n) + 1024);
                             n = g_script.get_state(buf.data(), (int)buf.size()); }
                if (n > 0) g_persistent_state_json.assign(buf.data(), (size_t)n);
            }
            xi::script::unload_script(g_script);
            std::string err;
            if (!xi::script::load_script(res.dll_path, g_script, err)) {
                send_rsp_err(srv, id, err);
                return;
            }
            // Wire xi::use() callbacks so the script can call back into backend
            if (g_script.set_use_callbacks) {
                g_script.set_use_callbacks(
                    (void*)use_process_cb,
                    (void*)use_exchange_cb,
                    (void*)use_grab_cb);
            }
            // Restore persistent state into the new DLL
            if (g_script.set_state && g_persistent_state_json.size() > 2) {
                g_script.set_state(g_persistent_state_json.c_str());
                std::fprintf(stderr, "[xinsp2] state restored (%zu bytes)\n",
                             g_persistent_state_json.size());
            }
        }

        // Build log can be large — send as a log message, not inline data.
        if (!res.build_log.empty()) {
            xp::LogMsg lm;
            lm.level = "info";
            lm.msg = res.build_log;
            srv.send_text(lm.to_json());
        }

        // Return success with dll path.
        std::string data = "{\"dll\":";
        xp::json_escape_into(data, res.dll_path);
        data += "}";
        send_rsp_ok(srv, id, data);
    } else if (name == "unload_script") {
        std::lock_guard<std::mutex> lk(g_script_mu);
        xi::script::unload_script(g_script);
        send_rsp_ok(srv, id);
    } else if (name == "run") {
        if (g_continuous.load()) {
            send_rsp_err(srv, id, "cannot run while continuous mode is active — stop first");
            return;
        }
        int64_t run_id = ++g_run_id;
        auto t0 = std::chrono::steady_clock::now();

        // Send rsp first (tests expect rsp before vars)
        // We don't know timing yet but run_id is ready.
        // Timing is reported via the run_finished event instead.
        char buf[128];
        std::snprintf(buf, sizeof(buf), R"({"run_id":%lld,"ms":0})", (long long)run_id);
        send_rsp_ok(srv, id, buf);

        // Run inspection — emits vars + previews.
        // SEH catches crashes; infinite loops need watchdog (future).
        run_one_inspection(srv, /*frame_hint=*/1, run_id);
    } else if (name == "start") {
        // Start continuous trigger mode. The backend runs a timer thread
        // that calls inspect() at a configurable interval. The script's
        // own ImageSource (if any) runs its acquisition thread inside
        // the DLL — the backend doesn't manage it.
        if (g_continuous.load()) {
            send_rsp_ok(srv, id, R"({"already":true})");
            return;
        }

        // Parse optional fps from args (default 10)
        int fps = 10;
        auto fps_val = xp::get_number_field(parsed->args_json, "fps");
        if (fps_val && *fps_val > 0) fps = (int)*fps_val;

        // Stop any existing worker before starting a new one
        if (g_worker_thread.joinable()) {
            g_continuous = false;
            g_worker_thread.join();
        }

        g_continuous = true;
        g_trigger_pending = 0;

        int interval_ms = 1000 / std::max(fps, 1);
        // Capture srv as pointer — it's a local in main(), outlives all threads
        auto* srv_ptr = &srv;
        g_worker_thread = std::thread([srv_ptr, interval_ms] {
            _set_se_translator(seh_translator); // SEH on worker thread too
            std::fprintf(stderr, "[xinsp2] continuous mode: %dms interval\n", interval_ms);
            int frame_seq = 0;
            while (g_continuous.load()) {
                run_one_inspection(*srv_ptr, frame_seq++);
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            }
            std::fprintf(stderr, "[xinsp2] continuous mode stopped\n");
        });

        send_rsp_ok(srv, id, R"({"started":true})");
    } else if (name == "stop") {
        g_continuous = false;
        g_trigger_cv.notify_all();
        if (g_worker_thread.joinable()) g_worker_thread.join();
        send_rsp_ok(srv, id, R"({"stopped":true})");
    } else if (name == "list_params") {
        // If a script is loaded, delegate to its own registry thunk so we
        // see the DLL's params. Otherwise report the backend's own.
        std::string params_json;
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.list_params) {
                std::vector<char> buf(64 * 1024);
                int n = g_script.list_params(buf.data(), (int)buf.size());
                if (n < 0) { buf.resize(static_cast<size_t>(-n) + 1024);
                             n = g_script.list_params(buf.data(), (int)buf.size()); }
                if (n > 0) params_json.assign(buf.data(), (size_t)n);
            }
        }
        if (params_json.empty()) {
            params_json = "[";
            auto list = xi::ParamRegistry::instance().list();
            for (size_t i = 0; i < list.size(); ++i) {
                if (i) params_json += ",";
                params_json += list[i]->as_json();
            }
            params_json += "]";
        }
        std::string out = "{\"type\":\"instances\",\"instances\":[],\"params\":";
        out += params_json;
        out += "}";
        send_rsp_ok(srv, id, "{}");
        srv.send_text(out);
    } else if (name == "set_param") {
        auto pname = xp::get_string_field(parsed->args_json, "name");
        if (!pname) {
            send_rsp_err(srv, id, "set_param: missing name");
            return;
        }
        // Try the loaded script first, then fall back to backend registry.
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.set_param) {
                // Extract raw value substring as a bare scalar.
                std::string val;
                auto num = xp::get_number_field(parsed->args_json, "value");
                if (num) { char nb[64]; std::snprintf(nb, sizeof(nb), "%g", *num); val = nb; }
                else {
                    if (parsed->args_json.find("\"value\":true")  != std::string::npos) val = "true";
                    if (parsed->args_json.find("\"value\":false") != std::string::npos) val = "false";
                }
                if (!val.empty()) {
                    int rc = g_script.set_param(pname->c_str(), val.c_str());
                    if (rc == 0) { send_rsp_ok(srv, id); return; }
                    // fall through to backend registry on -1 (not found)
                }
            }
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
    } else if (name == "list_instances") {
        std::string inst_json, params_json;
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok()) {
                std::vector<char> buf(64 * 1024);
                if (g_script.list_instances) {
                    int n = g_script.list_instances(buf.data(), (int)buf.size());
                    if (n < 0) { buf.resize((size_t)(-n) + 1024); n = g_script.list_instances(buf.data(), (int)buf.size()); }
                    if (n > 0) inst_json.assign(buf.data(), (size_t)n);
                }
                if (g_script.list_params) {
                    int n = g_script.list_params(buf.data(), (int)buf.size());
                    if (n < 0) { buf.resize((size_t)(-n) + 1024); n = g_script.list_params(buf.data(), (int)buf.size()); }
                    if (n > 0) params_json.assign(buf.data(), (size_t)n);
                }
            }
        }
        std::string out = "{\"type\":\"instances\",\"instances\":";
        out += inst_json.empty() ? "[]" : inst_json;
        out += ",\"params\":";
        out += params_json.empty() ? "[]" : params_json;
        out += "}";
        send_rsp_ok(srv, id, "{}");
        srv.send_text(out);
    } else if (name == "set_instance_def") {
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        // Extract the def object as a raw JSON substring
        std::string def_str;
        const char* after;
        if (xp::detail::find_key(parsed->args_json.data(),
                                  parsed->args_json.data() + parsed->args_json.size(),
                                  "def", def_str, after)) {
            // def_str is the raw JSON value
        } else {
            def_str = "{}";
        }
        std::lock_guard<std::mutex> lk(g_script_mu);
        if (g_script.ok() && g_script.set_instance_def) {
            int rc = g_script.set_instance_def(iname->c_str(), def_str.c_str());
            if (rc == 0) send_rsp_ok(srv, id);
            else         send_rsp_err(srv, id, "set_instance_def failed");
        } else {
            send_rsp_err(srv, id, "no script loaded");
        }
    } else if (name == "exchange_instance") {
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        std::string cmd_str;
        const char* after;
        if (xp::detail::find_key(parsed->args_json.data(),
                                  parsed->args_json.data() + parsed->args_json.size(),
                                  "cmd", cmd_str, after)) {
        } else {
            cmd_str = "{}";
        }
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        if (inst) {
            try {
                std::string result = inst->exchange(cmd_str);
                send_rsp_ok(srv, id, result);
            } catch (const seh_exception& e) {
                char msg[256];
                std::snprintf(msg, sizeof(msg), "exchange '%s' crashed: 0x%08X (%s)",
                             iname->c_str(), e.code, e.what());
                send_rsp_err(srv, id, msg);
            } catch (const std::exception& e) {
                send_rsp_err(srv, id, std::string("exchange error: ") + e.what());
            }
        } else {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.exchange_instance) {
                try {
                    std::vector<char> rsp(256 * 1024);
                    int n = g_script.exchange_instance(iname->c_str(), cmd_str.c_str(),
                                                       rsp.data(), (int)rsp.size());
                    if (n < 0) { rsp.resize((size_t)(-n) + 1024);
                                 n = g_script.exchange_instance(iname->c_str(), cmd_str.c_str(),
                                                                rsp.data(), (int)rsp.size()); }
                    if (n >= 0) send_rsp_ok(srv, id, std::string(rsp.data(), (size_t)n));
                    else        send_rsp_err(srv, id, "exchange_instance failed");
                } catch (const seh_exception& e) {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg), "script exchange '%s' crashed: 0x%08X (%s)",
                                 iname->c_str(), e.code, e.what());
                    send_rsp_err(srv, id, msg);
                }
            } else {
                send_rsp_err(srv, id, "instance not found: " + *iname);
            }
        }
    } else if (name == "save_project") {
        auto path = xp::get_string_field(parsed->args_json, "path");
        if (!path) { send_rsp_err(srv, id, "missing path"); return; }
        std::string params_json, inst_json;
        {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok()) {
                std::vector<char> buf(64 * 1024);
                if (g_script.list_params) {
                    int n = g_script.list_params(buf.data(), (int)buf.size());
                    if (n > 0) params_json.assign(buf.data(), (size_t)n);
                }
                if (g_script.list_instances) {
                    int n = g_script.list_instances(buf.data(), (int)buf.size());
                    if (n > 0) inst_json.assign(buf.data(), (size_t)n);
                }
            }
        }
        std::string content = xi::project::build_project_json(params_json, inst_json);
        if (xi::project::write_text(*path, content)) {
            send_rsp_ok(srv, id);
        } else {
            send_rsp_err(srv, id, "failed to write " + *path);
        }
    } else if (name == "load_project") {
        auto path = xp::get_string_field(parsed->args_json, "path");
        if (!path) { send_rsp_err(srv, id, "missing path"); return; }
        std::string content = xi::project::read_text(*path);
        if (content.empty()) { send_rsp_err(srv, id, "failed to read " + *path); return; }

        // Use cJSON to parse the project file properly
        cJSON* root = cJSON_Parse(content.c_str());
        if (!root) { send_rsp_err(srv, id, "invalid JSON in project file"); return; }

        // Restore params
        cJSON* params = cJSON_GetObjectItem(root, "params");
        if (params && cJSON_IsArray(params)) {
            int n = cJSON_GetArraySize(params);
            for (int i = 0; i < n; ++i) {
                cJSON* item = cJSON_GetArrayItem(params, i);
                cJSON* nm = cJSON_GetObjectItem(item, "name");
                cJSON* val = cJSON_GetObjectItem(item, "value");
                if (nm && cJSON_IsString(nm) && val) {
                    char vbuf[64] = {};
                    if (cJSON_IsNumber(val))     std::snprintf(vbuf, sizeof(vbuf), "%g", val->valuedouble);
                    else if (cJSON_IsBool(val))  std::snprintf(vbuf, sizeof(vbuf), "%s", cJSON_IsTrue(val) ? "true" : "false");
                    else continue;
                    // Try script params first, then backend params
                    std::lock_guard<std::mutex> lk(g_script_mu);
                    if (g_script.ok() && g_script.set_param) {
                        g_script.set_param(nm->valuestring, vbuf);
                    } else {
                        auto* p = xi::ParamRegistry::instance().find(nm->valuestring);
                        if (p) p->set_from_json(vbuf);
                    }
                }
            }
        }

        // Restore instance configs
        cJSON* instances = cJSON_GetObjectItem(root, "instances");
        if (instances && cJSON_IsArray(instances)) {
            int n = cJSON_GetArraySize(instances);
            for (int i = 0; i < n; ++i) {
                cJSON* item = cJSON_GetArrayItem(instances, i);
                cJSON* nm = cJSON_GetObjectItem(item, "name");
                cJSON* def = cJSON_GetObjectItem(item, "def");
                if (nm && cJSON_IsString(nm) && def) {
                    char* def_str = cJSON_PrintUnformatted(def);
                    auto inst = xi::InstanceRegistry::instance().find(nm->valuestring);
                    if (inst) inst->set_def(def_str);
                    cJSON_free(def_str);
                }
            }
        }

        cJSON_Delete(root);
        send_rsp_ok(srv, id);
    } else if (name == "preview_instance") {
        // Grab the latest frame from a named ImageSource instance,
        // JPEG-encode it, and send as a binary preview frame.
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        auto* src = inst ? dynamic_cast<xi::ImageSource*>(inst.get()) : nullptr;
        if (!src) { send_rsp_err(srv, id, "not an ImageSource: " + *iname); return; }

        xi::Image img = src->grab();
        if (img.empty()) { send_rsp_ok(srv, id, R"({"frame":false})"); return; }

        std::vector<uint8_t> jpeg;
        if (!xi::encode_jpeg(img, 80, jpeg)) { send_rsp_ok(srv, id, R"({"frame":false})"); return; }

        // Use a hash of the instance name as gid so the extension can
        // route the preview to the correct panel.
        uint32_t preview_gid = 9000;
        for (char c : *iname) preview_gid = preview_gid * 31 + (uint8_t)c;
        preview_gid = 9000 + (preview_gid % 1000);

        char gid_buf[64];
        std::snprintf(gid_buf, sizeof(gid_buf), R"({"frame":true,"gid":%u})", preview_gid);
        send_rsp_ok(srv, id, gid_buf);

        std::vector<uint8_t> frame(xp::kPreviewHeaderSize + jpeg.size());
        xp::PreviewHeader hd;
        hd.gid = preview_gid;
        hd.codec = (uint32_t)xp::Codec::JPEG;
        hd.width = (uint32_t)img.width;
        hd.height = (uint32_t)img.height;
        hd.channels = (uint32_t)img.channels;
        xp::encode_preview_header(hd, frame.data());
        std::memcpy(frame.data() + xp::kPreviewHeaderSize, jpeg.data(), jpeg.size());
        srv.send_binary(frame.data(), frame.size());
    } else if (name == "process_instance") {
        // Call a C ABI plugin's process() with an image from another instance.
        // args: { name: "detector0", source: "cam0", params: {...} }
        auto iname = xp::get_string_field(parsed->args_json, "name");
        auto source = xp::get_string_field(parsed->args_json, "source");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }

        // Find the plugin instance
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        auto* adapter = inst ? dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get()) : nullptr;
        if (!adapter || !adapter->process_fn()) {
            send_rsp_err(srv, id, "not a processable plugin: " + *iname);
            return;
        }

        // Get source image from an ImageSource instance
        xi::Image src_img;
        if (source) {
            auto src_inst = xi::InstanceRegistry::instance().find(*source);
            auto* img_src = src_inst ? dynamic_cast<xi::ImageSource*>(src_inst.get()) : nullptr;
            if (img_src) src_img = img_src->grab();
        }
        if (src_img.empty()) {
            send_rsp_err(srv, id, "no image available — provide a source instance or start streaming");
            return;
        }

        // Build input record with the image
        static xi_host_api host = xi::ImagePool::make_host_api();
        xi_image_handle src_h = xi::ImagePool::instance().from_image(src_img);

        // Parse extra params from args
        std::string params_json = "{}";
        const char* after;
        if (xp::detail::find_key(parsed->args_json.data(),
                                  parsed->args_json.data() + parsed->args_json.size(),
                                  "params", params_json, after)) {
            // params_json is already set
        }

        xi_record_image in_imgs[] = { {"gray", src_h} };
        xi_record input_rec;
        input_rec.images = in_imgs;
        input_rec.image_count = 1;
        input_rec.json = params_json.c_str();

        xi_record_out output;
        xi_record_out_init(&output);

        try {
            adapter->process_fn()(adapter->raw_instance(), &input_rec, &output);
        } catch (const seh_exception& e) {
            xi::ImagePool::instance().release(src_h);
            xi_record_out_free(&output);
            char msg[256];
            std::snprintf(msg, sizeof(msg), "process_instance '%s' crashed: 0x%08X (%s)",
                         iname->c_str(), e.code, e.what());
            send_rsp_err(srv, id, msg);
            return;
        }

        // Release input image handle (success path)
        xi::ImagePool::instance().release(src_h);

        // Build response: JSON data + send output images as preview frames
        std::string result_json = output.json ? output.json : "{}";

        // Add image info to result
        std::string full_json = result_json;
        if (output.image_count > 0) {
            // Insert images info into the JSON
            if (full_json.size() > 1 && full_json.back() == '}') {
                full_json.pop_back();
                if (full_json.size() > 1) full_json += ",";
                full_json += "\"_images\":{";
                for (int i = 0; i < output.image_count; ++i) {
                    if (i) full_json += ",";
                    uint32_t gid = 8000 + (uint32_t)i;
                    full_json += "\"" + std::string(output.images[i].key) + "\":" + std::to_string(gid);
                }
                full_json += "}}";
            }
        }

        send_rsp_ok(srv, id, full_json);

        // Send output images as binary preview frames
        for (int i = 0; i < output.image_count; ++i) {
            auto& oi = output.images[i];
            xi::Image out_img = xi::ImagePool::instance().to_image(oi.handle);
            // Always release the output handle — regardless of encode success
            xi::ImagePool::instance().release(oi.handle);

            if (out_img.empty()) continue;

            xi::Image jpeg_img = out_img;
            if (out_img.channels == 1) {
                jpeg_img = xi::Image(out_img.width, out_img.height, 3);
                for (int j = 0; j < out_img.width * out_img.height; ++j) {
                    jpeg_img.data()[j*3+0] = out_img.data()[j];
                    jpeg_img.data()[j*3+1] = out_img.data()[j];
                    jpeg_img.data()[j*3+2] = out_img.data()[j];
                }
            }

            std::vector<uint8_t> jpeg;
            if (!xi::encode_jpeg(jpeg_img, 85, jpeg)) continue;

            uint32_t gid = 8000 + (uint32_t)i;
            std::vector<uint8_t> frame(xp::kPreviewHeaderSize + jpeg.size());
            xp::PreviewHeader hd;
            hd.gid = gid;
            hd.codec = (uint32_t)xp::Codec::JPEG;
            hd.width = (uint32_t)out_img.width;
            hd.height = (uint32_t)out_img.height;
            hd.channels = (uint32_t)jpeg_img.channels;
            xp::encode_preview_header(hd, frame.data());
            std::memcpy(frame.data() + xp::kPreviewHeaderSize, jpeg.data(), jpeg.size());
            srv.send_binary(frame.data(), frame.size());
        }

        xi_record_out_free(&output);
    } else if (name == "list_plugins") {
        auto plugins = g_plugin_mgr.list_plugins();
        std::string out = "[";
        for (size_t i = 0; i < plugins.size(); ++i) {
            if (i) out += ",";
            auto& p = plugins[i];
            out += "{\"name\":\"" + p.name + "\",\"description\":\"" + p.description + "\"";
            out += ",\"has_ui\":" + std::string(p.has_ui ? "true" : "false");
            out += ",\"loaded\":" + std::string(p.handle ? "true" : "false") + "}";
        }
        out += "]";
        send_rsp_ok(srv, id, out);
    } else if (name == "load_plugin") {
        auto pname = xp::get_string_field(parsed->args_json, "name");
        if (!pname) { send_rsp_err(srv, id, "missing name"); return; }
        if (g_plugin_mgr.load_plugin(*pname)) {
            send_rsp_ok(srv, id);
        } else {
            send_rsp_err(srv, id, "failed to load plugin: " + *pname);
        }
    } else if (name == "create_project") {
        auto folder = xp::get_string_field(parsed->args_json, "folder");
        auto pname  = xp::get_string_field(parsed->args_json, "name");
        if (!folder || !pname) { send_rsp_err(srv, id, "missing folder or name"); return; }
        if (g_plugin_mgr.create_project(*folder, *pname)) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "failed to create project");
        }
    } else if (name == "open_project") {
        auto folder = xp::get_string_field(parsed->args_json, "folder");
        if (!folder) { send_rsp_err(srv, id, "missing folder"); return; }
        if (g_plugin_mgr.open_project(*folder)) {
            auto& proj = g_plugin_mgr.project();
            int inst_count = (int)proj.instances.size();
            std::fprintf(stderr, "[xinsp2] project opened: %s (%d instances)\n",
                         proj.name.c_str(), inst_count);
            for (auto& [k, v] : proj.instances) {
                std::fprintf(stderr, "[xinsp2]   instance: %s (%s)\n",
                             k.c_str(), v.plugin_name.c_str());
            }
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "failed to open project in " + *folder);
        }
    } else if (name == "create_instance") {
        auto iname  = xp::get_string_field(parsed->args_json, "name");
        auto plugin = xp::get_string_field(parsed->args_json, "plugin");
        if (!iname || !plugin) { send_rsp_err(srv, id, "missing name or plugin"); return; }
        // Ensure plugin is loaded
        g_plugin_mgr.load_plugin(*plugin);
        auto* ii = g_plugin_mgr.create_instance(*iname, *plugin);
        if (ii) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "failed to create instance");
        }
    } else if (name == "get_project") {
        send_rsp_ok(srv, id, g_plugin_mgr.to_json());
    } else if (name == "save_instance_config") {
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        if (g_plugin_mgr.save_instance(*iname)) {
            send_rsp_ok(srv, id);
        } else {
            send_rsp_err(srv, id, "instance not found: " + *iname);
        }
    } else if (name == "get_plugin_ui") {
        // Return the path to the plugin's UI folder so the extension can
        // load it into a webview.
        auto plugin = xp::get_string_field(parsed->args_json, "plugin");
        if (!plugin) { send_rsp_err(srv, id, "missing plugin"); return; }
        auto* pi = g_plugin_mgr.find_plugin(*plugin);
        if (pi && pi->has_ui) {
            std::string data = "{\"ui_path\":";
            xp::json_escape_into(data, pi->ui_path);
            data += "}";
            send_rsp_ok(srv, id, data);
        } else {
            send_rsp_err(srv, id, "no UI for plugin: " + *plugin);
        }
    } else {
        send_rsp_err(srv, id, std::string("unknown command: ") + name);
    }
}

int main(int argc, char** argv) {
    // Install SEH → C++ exception translator so try/catch catches segfaults
    _set_se_translator(seh_translator);

    int port = parse_port(argc, argv);

    // Derive include dir for the script compiler. In a normal dev tree the
    // backend .exe is at backend/build/Release, and headers are at
    // backend/include. Walk up until we find xi/xi.hpp.
    {
        std::filesystem::path p = get_exe_dir();
        for (int i = 0; i < 6; ++i) {
            if (std::filesystem::exists(p / "include" / "xi" / "xi.hpp")) {
                g_include_dir = (p / "include").string();
                break;
            }
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
        if (g_include_dir.empty()) {
            // Fallback: next to the exe.
            g_include_dir = (std::filesystem::path(get_exe_dir()) / "include").string();
        }
    }
    g_work_dir = (std::filesystem::temp_directory_path() / "xinsp2").string();
    std::filesystem::create_directories(g_work_dir);

    // Find and scan plugins directory (sibling of backend/)
    {
        std::filesystem::path p = get_exe_dir();
        for (int i = 0; i < 6; ++i) {
            if (std::filesystem::exists(p / "plugins")) {
                g_plugins_dir = (p / "plugins").string();
                break;
            }
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
    }
    if (!g_plugins_dir.empty()) {
        int n = g_plugin_mgr.scan_plugins(g_plugins_dir);
        std::fprintf(stderr, "[xinsp2] scanned %d plugins from %s\n", n, g_plugins_dir.c_str());
    }

    std::fprintf(stderr, "[xinsp2] include_dir=%s\n", g_include_dir.c_str());
    std::fprintf(stderr, "[xinsp2] work_dir=%s\n",    g_work_dir.c_str());
    std::fprintf(stderr, "[xinsp2] plugins_dir=%s\n",  g_plugins_dir.c_str());

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
