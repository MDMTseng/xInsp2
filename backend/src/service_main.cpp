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
#include <unordered_set>

#include <cJSON.h>
#include <xi/xi.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_jpeg.hpp>
#include <xi/xi_protocol.hpp>
#include <xi/xi_cert.hpp>
#include <xi/xi_plugin_manager.hpp>
#include <xi/xi_project.hpp>
#include <xi/xi_trigger_bus.hpp>
#include <xi/xi_trigger_bridge.hpp>
#include <xi/xi_trigger_recorder.hpp>
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
static std::thread             g_worker_thread;

// Preview subscription (S1). Default: send every image VAR's JPEG after
// a run (back-compat). Client sets a name allow-list via cmd:subscribe
// to cut bandwidth for vars nobody is watching. Held under g_sub_mu so
// the WS thread (who mutates it) and the run dispatch thread (who reads)
// stay consistent.
static std::mutex                    g_sub_mu;
static bool                          g_sub_all = true;
static std::unordered_set<std::string> g_sub_names;

// ---- Trigger access (script callbacks) ---------------------------------
// Set by the worker thread (or run_one_inspection) before invoking the
// script. The script reads via xi::current_trigger() through the three
// trigger_*_cb functions below. thread_local so multiple parallel
// dispatch threads can each have their own current trigger.
static thread_local const xi::TriggerEvent* g_current_trigger = nullptr;

// Bus event queue feeding the continuous-mode worker. Bus sink pushes
// events here; worker pops, dispatches, releases handles.
static std::deque<xi::TriggerEvent> g_ev_queue;
static std::mutex                   g_ev_mu;
static std::condition_variable      g_ev_cv;

struct CurrentTriggerInfoC {        // mirrors xi::CurrentTriggerInfo (xi_use.hpp)
    xi_trigger_id id;
    int64_t       timestamp_us;
    int32_t       is_active;
};

static void trigger_info_cb(CurrentTriggerInfoC* out) {
    if (!out) return;
    if (!g_current_trigger) { *out = {{0,0}, 0, 0}; return; }
    out->id           = g_current_trigger->id;
    out->timestamp_us = g_current_trigger->timestamp_us;
    out->is_active    = 1;
}

static xi_image_handle trigger_image_cb(const char* source) {
    if (!g_current_trigger || !source) return XI_IMAGE_NULL;
    auto it = g_current_trigger->images.find(source);
    if (it == g_current_trigger->images.end()) return XI_IMAGE_NULL;
    // Caller (script) releases via host_api->image_release after copying
    // pixels — addref so our own release on dispatch-end doesn't free it
    // out from under them.
    xi::ImagePool::instance().addref(it->second);
    return it->second;
}

static int32_t trigger_sources_cb(char* buf, int32_t buflen) {
    if (!g_current_trigger || !buf) return 0;
    std::string out;
    bool first = true;
    for (auto& [src, h] : g_current_trigger->images) {
        if (!first) out.push_back('\n');
        first = false;
        out += src;
    }
    int32_t n = (int32_t)out.size();
    if (buflen < n + 1) return -n;
    std::memcpy(buf, out.data(), n);
    buf[n] = 0;
    return n;
}

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

// --host=<addr>  (default 127.0.0.1). Pass 0.0.0.0 for remote-reachable.
static std::string parse_host(int argc, char** argv) {
    std::string host = "127.0.0.1";
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind("--host=", 0) == 0) host = std::string(a.substr(7));
        else if (a == "--host" && i + 1 < argc) host = argv[++i];
    }
    if (const char* env = std::getenv("XINSP2_HOST"); env && *env) host = env;
    return host;
}

// --auth=<secret>  (default empty = no auth required).
// Also XINSP2_AUTH env var (preferred on shared servers — no argv leak to ps).
static std::string parse_auth_secret(int argc, char** argv) {
    std::string secret;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind("--auth=", 0) == 0) secret = std::string(a.substr(7));
        else if (a == "--auth" && i + 1 < argc) secret = argv[++i];
    }
    if (const char* env = std::getenv("XINSP2_AUTH"); env && *env) secret = env;
    return secret;
}

// Repeatable: --plugins-dir=/some/path  (or --plugins-dir /some/path).
// Also reads XINSP2_EXTRA_PLUGIN_DIRS, semicolon- or path-separator-delimited.
static std::vector<std::string> parse_extra_plugin_dirs(int argc, char** argv) {
    std::vector<std::string> dirs;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind("--plugins-dir=", 0) == 0) {
            dirs.emplace_back(std::string(a.substr(14)));
        } else if (a == "--plugins-dir" && i + 1 < argc) {
            dirs.emplace_back(argv[++i]);
        }
    }
    if (const char* env = std::getenv("XINSP2_EXTRA_PLUGIN_DIRS")) {
        std::string s(env);
        size_t start = 0;
        while (start <= s.size()) {
            size_t end = s.find_first_of(";,", start);
            if (end == std::string::npos) end = s.size();
            std::string item = s.substr(start, end - start);
            if (!item.empty()) dirs.push_back(std::move(item));
            if (end == s.size()) break;
            start = end + 1;
        }
    }
    return dirs;
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

        // image previews — filtered by subscription. For each
        // `"name":"X"` followed by `"gid":N`, only emit the JPEG when
        // the subscription set allows it (or we're in send-all mode).
        bool sub_all;
        std::unordered_set<std::string> sub_names;
        {
            std::lock_guard<std::mutex> lk(g_sub_mu);
            sub_all = g_sub_all;
            if (!sub_all) sub_names = g_sub_names;   // copy under lock
        }

        std::string_view snap_view(sbuf.data(), (size_t)n);
        size_t pos = 0;
        std::string cur_name;
        while (pos < snap_view.size()) {
            // Track the latest `"name":"..."` we saw; every later `"gid":`
            // is assumed to belong to that entry (snapshot emits name
            // before gid within each item).
            auto nm = snap_view.find("\"name\":\"", pos);
            auto gd = snap_view.find("\"gid\":", pos);
            if (gd == std::string_view::npos) break;
            if (nm != std::string_view::npos && nm < gd) {
                nm += 8;
                auto end = snap_view.find('"', nm);
                if (end != std::string_view::npos) cur_name = std::string(snap_view.substr(nm, end - nm));
            }
            pos = gd + 6;
            uint32_t gid = (uint32_t)std::atoi(snap_view.data() + pos);
            if (!sub_all && !sub_names.count(cur_name)) continue;
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
    } else if (name == "subscribe") {
        // args: { names: [...] } OR { all: true }
        bool want_all = parsed->args_json.find("\"all\":true") != std::string::npos;
        std::unordered_set<std::string> names;
        if (!want_all) {
            // Parse the names array. cJSON is simpler than hand-rolled here.
            cJSON* root = cJSON_Parse(parsed->args_json.c_str());
            if (root) {
                cJSON* arr = cJSON_GetObjectItem(root, "names");
                if (cJSON_IsArray(arr)) {
                    int n = cJSON_GetArraySize(arr);
                    for (int i = 0; i < n; ++i) {
                        cJSON* it = cJSON_GetArrayItem(arr, i);
                        if (cJSON_IsString(it) && it->valuestring) names.insert(it->valuestring);
                    }
                }
                cJSON_Delete(root);
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_sub_mu);
            g_sub_all = want_all;
            g_sub_names = std::move(names);
        }
        std::string out = "{\"all\":";
        out += want_all ? "true" : "false";
        out += ",\"count\":";
        {
            std::lock_guard<std::mutex> lk(g_sub_mu);
            out += std::to_string(g_sub_names.size());
        }
        out += "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "unsubscribe") {
        {
            std::lock_guard<std::mutex> lk(g_sub_mu);
            g_sub_all = false;
            g_sub_names.clear();
        }
        send_rsp_ok(srv, id, R"({"all":false,"count":0})");
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
            // Wire xi::use() callbacks so the script can call back into backend.
            // host_api lets the script allocate/read images in the BACKEND pool —
            // plugins only see that pool via their own host_api, so script-side
            // ImagePool handles would be invisible to them.
            if (g_script.set_use_callbacks) {
                static xi_host_api use_host = []{ auto a = xi::ImagePool::make_host_api(); xi::install_trigger_hook(a); return a; }();
                g_script.set_use_callbacks(
                    (void*)use_process_cb,
                    (void*)use_exchange_cb,
                    (void*)use_grab_cb,
                    (void*)&use_host);
            }
            // Phase 3: trigger access callbacks. Older scripts that don't
            // import xi_script_set_trigger_callbacks just stay null and
            // xi::current_trigger().is_active() returns false.
            if (g_script.set_trigger_callbacks) {
                g_script.set_trigger_callbacks(
                    (void*)trigger_info_cb,
                    (void*)trigger_image_cb,
                    (void*)trigger_sources_cb);
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

        int interval_ms = 1000 / std::max(fps, 1);
        auto* srv_ptr = &srv;

        // Bus-driven worker: events arrive via TriggerBus sink → enqueued
        // → worker pops and runs inspect with that trigger as current.
        // Timer fallback fires too (legacy back-compat) so scripts whose
        // sources don't emit_trigger still see periodic dispatch.
        xi::TriggerBus::instance().set_sink([](xi::TriggerEvent ev) {
            std::lock_guard<std::mutex> lk(g_ev_mu);
            g_ev_queue.push_back(std::move(ev));
            g_ev_cv.notify_one();
        });

        g_worker_thread = std::thread([srv_ptr, interval_ms] {
            _set_se_translator(seh_translator);
            std::fprintf(stderr, "[xinsp2] continuous mode: %dms timer + trigger bus\n",
                         interval_ms);
            int frame_seq = 0;
            while (g_continuous.load()) {
                xi::TriggerEvent ev;
                bool have_ev = false;
                {
                    std::unique_lock<std::mutex> lk(g_ev_mu);
                    g_ev_cv.wait_for(lk, std::chrono::milliseconds(interval_ms),
                                     [] { return !g_ev_queue.empty() || !g_continuous.load(); });
                    if (!g_continuous.load()) break;
                    if (!g_ev_queue.empty()) {
                        ev = std::move(g_ev_queue.front());
                        g_ev_queue.pop_front();
                        have_ev = true;
                    }
                }
                if (have_ev) {
                    g_current_trigger = &ev;
                    run_one_inspection(*srv_ptr, frame_seq++);
                    g_current_trigger = nullptr;
                    for (auto& [src, h] : ev.images) xi::ImagePool::instance().release(h);
                } else {
                    // Timer-only tick: legacy back-compat dispatch.
                    run_one_inspection(*srv_ptr, frame_seq++);
                }
            }
            std::fprintf(stderr, "[xinsp2] continuous mode stopped\n");
        });

        send_rsp_ok(srv, id, R"({"started":true})");
    } else if (name == "stop") {
        g_continuous = false;
        g_ev_cv.notify_all();           // wake bus-driven worker
        xi::TriggerBus::instance().clear_sink();
        if (g_worker_thread.joinable()) g_worker_thread.join();
        // Drain any in-flight events that arrived between sink-clear and join.
        {
            std::lock_guard<std::mutex> lk(g_ev_mu);
            for (auto& ev : g_ev_queue) {
                for (auto& [src, h] : ev.images) xi::ImagePool::instance().release(h);
            }
            g_ev_queue.clear();
        }
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
        // Also include backend-managed instances (from PluginManager)
        auto& proj = g_plugin_mgr.project();
        std::string backend_inst = "[";
        int bi = 0;
        for (auto& [k, v] : proj.instances) {
            if (bi++) backend_inst += ",";
            backend_inst += "{\"name\":\"" + v.name + "\",\"plugin\":\"" + v.plugin_name + "\"}";
        }
        backend_inst += "]";

        // Merge: script instances + backend instances
        std::string merged_inst;
        if (!inst_json.empty() && inst_json != "[]" && bi > 0) {
            // Both have entries — merge arrays
            merged_inst = inst_json.substr(0, inst_json.size() - 1) + "," + backend_inst.substr(1);
        } else if (bi > 0) {
            merged_inst = backend_inst;
        } else {
            merged_inst = inst_json.empty() ? "[]" : inst_json;
        }

        std::string out = "{\"type\":\"instances\",\"instances\":";
        out += merged_inst;
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
        // Try backend's InstanceRegistry first (plugin-manager instances)
        auto inst = xi::InstanceRegistry::instance().find(*iname);
        if (inst) {
            if (inst->set_def(def_str)) send_rsp_ok(srv, id);
            else send_rsp_err(srv, id, "set_def returned false");
        } else {
            std::lock_guard<std::mutex> lk(g_script_mu);
            if (g_script.ok() && g_script.set_instance_def) {
                int rc = g_script.set_instance_def(iname->c_str(), def_str.c_str());
                if (rc == 0) send_rsp_ok(srv, id);
                else         send_rsp_err(srv, id, "set_instance_def failed");
            } else {
                send_rsp_err(srv, id, "instance not found: " + *iname);
            }
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
        } catch (const std::exception& e) {
            xi::ImagePool::instance().release(src_h);
            xi_record_out_free(&output);
            send_rsp_err(srv, id, std::string("process_instance '") + *iname +
                                  "' threw: " + e.what());
            return;
        } catch (...) {
            xi::ImagePool::instance().release(src_h);
            xi_record_out_free(&output);
            send_rsp_err(srv, id, "process_instance '" + *iname + "' threw unknown exception");
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
        auto esc = [](const std::string& s) {
            std::string o; for (char c : s) { if (c=='\\'||c=='"') o.push_back('\\'); o.push_back(c); } return o;
        };
        for (size_t i = 0; i < plugins.size(); ++i) {
            if (i) out += ",";
            auto& p = plugins[i];
            out += "{\"name\":\"" + esc(p.name) + "\",\"description\":\"" + esc(p.description) + "\"";
            out += ",\"folder\":\"" + esc(p.folder_path) + "\"";
            out += ",\"has_ui\":" + std::string(p.has_ui ? "true" : "false");
            out += ",\"loaded\":" + std::string(p.handle ? "true" : "false");
            // Cert snapshot (doesn't re-run the tests — just reads cert.json if present)
            xi::cert::Cert c;
            if (xi::cert::read(p.folder_path, c)) {
                auto dll_path = std::filesystem::path(p.folder_path) / p.dll_name;
                bool valid = xi::cert::is_valid(p.folder_path, dll_path);
                out += ",\"cert\":{\"present\":true,\"valid\":" + std::string(valid ? "true" : "false");
                out += ",\"baseline_version\":" + std::to_string(c.baseline_version);
                out += ",\"certified_at\":\"" + esc(c.certified_at) + "\"}";
            } else {
                out += ",\"cert\":{\"present\":false}";
            }
            out += "}";
        }
        out += "]";
        send_rsp_ok(srv, id, out);
    } else if (name == "rescan_plugins") {
        // Optional arg: {"dir": "<path>"} scans that one dir (additive).
        // No arg: re-scan the default plugins_dir.
        auto dir_opt = xp::get_string_field(parsed->args_json, "dir");
        const std::string& dir = dir_opt ? *dir_opt : g_plugins_dir;
        int n = 0;
        if (!dir.empty() && std::filesystem::exists(dir)) {
            n = g_plugin_mgr.scan_plugins(dir);
        }
        std::string out = "{\"scanned\":";
        xp::json_escape_into(out, dir);
        out += ",\"count\":" + std::to_string(n) + "}";
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
    } else if (name == "close_project") {
        g_plugin_mgr.close_project();
        send_rsp_ok(srv, id, "{\"closed\":true}");
    } else if (name == "recording_start") {
        auto folder = xp::get_string_field(parsed->args_json, "folder");
        if (!folder) { send_rsp_err(srv, id, "missing folder"); return; }
        if (xi::TriggerRecorder::instance().start(*folder)) {
            std::string out = "{\"recording\":true,\"folder\":";
            xp::json_escape_into(out, *folder);
            out += "}";
            send_rsp_ok(srv, id, out);
        } else {
            send_rsp_err(srv, id, "already recording");
        }
    } else if (name == "recording_stop") {
        bool ok = xi::TriggerRecorder::instance().stop();
        std::string out = "{\"recording\":false,\"events\":" +
            std::to_string(xi::TriggerRecorder::instance().event_count()) + "}";
        send_rsp_ok(srv, id, out);
        (void)ok;
    } else if (name == "recording_status") {
        std::string out = "{\"recording\":";
        out += xi::TriggerRecorder::instance().is_recording() ? "true" : "false";
        out += ",\"replaying\":";
        out += xi::TriggerRecorder::instance().is_replaying() ? "true" : "false";
        out += ",\"events\":" + std::to_string(xi::TriggerRecorder::instance().event_count());
        out += ",\"folder\":";
        xp::json_escape_into(out, xi::TriggerRecorder::instance().folder());
        out += "}";
        send_rsp_ok(srv, id, out);
    } else if (name == "recording_replay") {
        auto folder = xp::get_string_field(parsed->args_json, "folder");
        if (!folder) { send_rsp_err(srv, id, "missing folder"); return; }
        auto speed = xp::get_number_field(parsed->args_json, "speed").value_or(1.0);
        if (xi::TriggerRecorder::instance().start_replay(*folder, speed)) {
            send_rsp_ok(srv, id, "{\"started\":true}");
        } else {
            send_rsp_err(srv, id, "could not start replay (no manifest, or already replaying)");
        }
    } else if (name == "set_trigger_policy") {
        // args: { policy: "any"|"all_required"|"leader_followers",
        //         required: ["cam_left", ...],
        //         leader: "cam_left",
        //         window_ms: 100 }
        auto pol_str = xp::get_string_field(parsed->args_json, "policy");
        xi::TriggerPolicy pol = xi::TriggerPolicy::Any;
        if      (pol_str && *pol_str == "all_required")     pol = xi::TriggerPolicy::AllRequired;
        else if (pol_str && *pol_str == "leader_followers") pol = xi::TriggerPolicy::LeaderFollowers;
        // Super-minimal array parse — required sources extracted by scanning.
        std::vector<std::string> required;
        auto rp = parsed->args_json.find("\"required\":[");
        if (rp != std::string::npos) {
            auto end = parsed->args_json.find(']', rp);
            if (end != std::string::npos) {
                std::string section = parsed->args_json.substr(rp + 12, end - (rp + 12));
                size_t pos = 0;
                while (pos < section.size()) {
                    auto q1 = section.find('"', pos); if (q1 == std::string::npos) break;
                    auto q2 = section.find('"', q1 + 1); if (q2 == std::string::npos) break;
                    required.emplace_back(section.substr(q1 + 1, q2 - q1 - 1));
                    pos = q2 + 1;
                }
            }
        }
        auto leader = xp::get_string_field(parsed->args_json, "leader").value_or("");
        auto win    = xp::get_number_field(parsed->args_json, "window_ms").value_or(100);
        if (g_plugin_mgr.set_trigger_policy(pol, required, leader, (int)win)) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "no project open");
        }
    } else if (name == "recertify_plugin") {
        auto pname = xp::get_string_field(parsed->args_json, "name");
        if (!pname) { send_rsp_err(srv, id, "missing name"); return; }
        auto* pi = g_plugin_mgr.find_plugin(*pname);
        if (!pi) { send_rsp_err(srv, id, "unknown plugin: " + *pname); return; }
        // Delete existing cert so load_plugin re-runs baseline on next scan.
        auto cert_path = std::filesystem::path(pi->folder_path) / "cert.json";
        std::error_code ec;
        std::filesystem::remove(cert_path, ec);
        // If currently loaded, run baseline now and write cert in-place.
        if (pi->handle) {
            auto syms = xi::baseline::load_symbols(pi->handle);
            static xi_host_api host = xi::ImagePool::make_host_api();
            auto summary = xi::cert::certify(pi->folder_path,
                std::filesystem::path(pi->folder_path) / pi->dll_name,
                pi->name, syms, &host);
            std::string rsp_json = "{\"passed\":" + std::string(summary.all_passed ? "true" : "false");
            rsp_json += ",\"pass_count\":" + std::to_string(summary.pass_count);
            rsp_json += ",\"fail_count\":" + std::to_string(summary.fail_count);
            rsp_json += ",\"total_ms\":" + std::to_string(summary.total_ms);
            rsp_json += ",\"failures\":[";
            bool first = true;
            for (auto& r : summary.results) {
                if (!r.passed) {
                    if (!first) rsp_json += ",";
                    first = false;
                    auto esc = [](const std::string& s) {
                        std::string o; for (char c : s) { if (c=='\\'||c=='"') o.push_back('\\'); o.push_back(c); } return o;
                    };
                    rsp_json += "{\"name\":\"" + esc(r.name) + "\",\"error\":\"" + esc(r.error) + "\"}";
                }
            }
            rsp_json += "]}";
            send_rsp_ok(srv, id, rsp_json);
        } else {
            send_rsp_ok(srv, id, "{\"queued\":true,\"note\":\"will re-cert on next load\"}");
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
    } else if (name == "remove_instance") {
        auto iname = xp::get_string_field(parsed->args_json, "name");
        if (!iname) { send_rsp_err(srv, id, "missing name"); return; }
        bool delete_folder =
            parsed->args_json.find("\"delete_folder\":true") != std::string::npos;
        if (g_plugin_mgr.remove_instance(*iname, delete_folder)) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "instance not found: " + *iname);
        }
    } else if (name == "rename_instance") {
        auto old_name = xp::get_string_field(parsed->args_json, "name");
        auto new_name = xp::get_string_field(parsed->args_json, "new_name");
        if (!old_name || !new_name) { send_rsp_err(srv, id, "missing name or new_name"); return; }
        if (g_plugin_mgr.rename_instance(*old_name, *new_name)) {
            send_rsp_ok(srv, id, g_plugin_mgr.to_json());
        } else {
            send_rsp_err(srv, id, "rename failed — name in use or instance missing");
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
    // Additional plugin folders from --plugins-dir / XINSP2_EXTRA_PLUGIN_DIRS.
    // Lets external SDKs keep their plugin DLLs in place — no copy needed.
    for (auto& dir : parse_extra_plugin_dirs(argc, argv)) {
        if (!std::filesystem::exists(dir)) {
            std::fprintf(stderr, "[xinsp2] extra plugin dir not found: %s\n", dir.c_str());
            continue;
        }
        int n = g_plugin_mgr.scan_plugins(dir);
        std::fprintf(stderr, "[xinsp2] scanned %d plugins from %s\n", n, dir.c_str());
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

    std::string host   = parse_host(argc, argv);
    std::string secret = parse_auth_secret(argc, argv);
    srv.set_bind_host(host);
    if (!secret.empty()) srv.set_auth_secret(secret);

    if (!srv.start(port)) {
        std::fprintf(stderr, "[xinsp2] failed to bind %s:%d\n", host.c_str(), port);
        return 1;
    }
    if (host == "0.0.0.0" && secret.empty()) {
        std::fprintf(stderr,
            "[xinsp2] WARNING: bound to 0.0.0.0 with NO --auth secret — anyone reachable can drive the backend\n");
    }
    std::fprintf(stderr, "[xinsp2] listening on ws://%s:%d%s\n",
                 host.c_str(), port,
                 secret.empty() ? "" : " (auth required)");
    std::fflush(stderr);

    while (!g_should_exit.load() && srv.is_running()) {
        srv.poll(100);
    }

    srv.stop();
    std::fprintf(stderr, "[xinsp2] shutdown complete\n");
    return 0;
}
