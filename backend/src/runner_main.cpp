//
// runner_main.cpp — xinsp-runner.exe
//
// Headless production runner. Takes a project folder, compiles the
// inspection script if needed, restores all instances, and runs
// inspect() N times. Writes a JSON report with per-frame variable
// snapshots.
//
// No WebSocket. No GUI. No plugins beyond what the project uses. The
// smallest possible binary that turns "saved project" → "pass/fail log".
//
// Usage:
//   xinsp-runner.exe <project-folder> [--frames=N] [--output=report.json]
//                                     [--script=path.cpp] [--plugins-dir=...]
//
// Example:
//   xinsp-runner.exe C:\factory\project --frames=1000 --output=today.json
//
// Exit: 0 if all frames dispatched; 1 on compile/load failure or any
// script crash. The caller is expected to grep the report for its own
// pass/fail field.
//

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <xi/xi_abi.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_instance.hpp>
#include <xi/xi_plugin_manager.hpp>
#include <xi/xi_protocol.hpp>
#include <xi/xi_script_compiler.hpp>
#include <xi/xi_script_loader.hpp>
#include <xi/xi_source.hpp>
#include <xi/xi_trigger_bus.hpp>
#include <xi/xi_trigger_bridge.hpp>

#include <windows.h>
#include <eh.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// --- SEH translator so backend survives script crashes ------------------

class seh_exception : public std::exception {
public:
    unsigned int code;
    explicit seh_exception(unsigned int c) : code(c) {}
    const char* what() const noexcept override { return "seh"; }
};

static void seh_translator(unsigned int code, EXCEPTION_POINTERS*) {
    throw seh_exception(code);
}

// --- xi::use() callbacks (minimal copy of service_main equivalents) -----

static int use_process_cb(const char* name,
                          const char* input_json,
                          const xi_record_image* images, int image_count,
                          xi_record_out* output) {
    auto inst = xi::InstanceRegistry::instance().find(name);
    if (!inst) return -1;
    auto* adapter = dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get());
    if (adapter && adapter->process_fn()) {
        xi_record in_rec{ images, image_count, input_json };
        try {
            adapter->process_fn()(adapter->raw_instance(), &in_rec, output);
        } catch (...) { return -2; }
        return output->image_count;
    }
    return -1;
}

static int use_exchange_cb(const char* name, const char* cmd,
                           char* rsp, int rsplen) {
    try {
        auto inst = xi::InstanceRegistry::instance().find(name);
        if (!inst) return -1;
        std::string out = inst->exchange(cmd);
        int n = (int)out.size();
        if (rsplen < n + 1) return -n;
        std::memcpy(rsp, out.data(), out.size());
        rsp[out.size()] = 0;
        return n;
    } catch (...) { return -1; }
}

static xi_image_handle use_grab_cb(const char* name, int timeout_ms) {
    auto inst = xi::InstanceRegistry::instance().find(name);
    auto* src = inst ? dynamic_cast<xi::ImageSource*>(inst.get()) : nullptr;
    if (!src) return XI_IMAGE_NULL;
    xi::Image img = src->grab_wait(timeout_ms);
    if (img.empty()) return XI_IMAGE_NULL;
    return xi::ImagePool::instance().from_image(img);
}

// --- args ---------------------------------------------------------------

struct Args {
    std::string project_dir;
    std::string output = "report.json";
    std::string script_override;       // --script=path; else project/inspection.cpp
    std::vector<std::string> extra_plugins;
    int         frames = 10;
    bool        help = false;
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view s = argv[i];
        if (s == "-h" || s == "--help") { a.help = true; continue; }
        auto take = [&](std::string_view flag) -> const char* {
            if (s.rfind(flag, 0) == 0 && s.size() > flag.size() && s[flag.size()] == '=')
                return argv[i] + flag.size() + 1;
            if (s == flag && i + 1 < argc) return argv[++i];
            return nullptr;
        };
        if (auto v = take("--frames"))       { try { a.frames = std::stoi(v); } catch (...) {} continue; }
        if (auto v = take("--output"))       { a.output = v; continue; }
        if (auto v = take("--script"))       { a.script_override = v; continue; }
        if (auto v = take("--plugins-dir"))  { a.extra_plugins.emplace_back(v); continue; }
        if (!s.empty() && s[0] != '-' && a.project_dir.empty()) {
            a.project_dir = argv[i];
        }
    }
    return a;
}

static void print_usage() {
    std::fprintf(stderr,
        "xinsp-runner — headless inspection runner\n"
        "\n"
        "Usage:\n"
        "  xinsp-runner <project-folder> [options]\n"
        "\n"
        "Options:\n"
        "  --frames=N         number of inspect() calls (default: 10)\n"
        "  --output=PATH      JSON report path (default: report.json)\n"
        "  --script=PATH      override inspection source (default: <proj>/inspection.cpp)\n"
        "  --plugins-dir=DIR  extra plugin folder (repeatable)\n"
        "  -h, --help         show this help\n");
}

// --- main ---------------------------------------------------------------

static std::string get_exe_dir() {
    char buf[MAX_PATH]; DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0) return {};
    std::string s(buf, n);
    auto slash = s.find_last_of("\\/");
    return (slash == std::string::npos) ? "." : s.substr(0, slash);
}

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    if (args.help || args.project_dir.empty()) {
        print_usage();
        return args.help ? 0 : 2;
    }
    if (!fs::exists(args.project_dir)) {
        std::fprintf(stderr, "[runner] project folder not found: %s\n", args.project_dir.c_str());
        return 2;
    }

    _set_se_translator(seh_translator);

    // Resolve xInsp2 include dir + built-in plugins by walking up from exe.
    std::string include_dir, plugins_dir;
    {
        fs::path p = get_exe_dir();
        for (int i = 0; i < 6; ++i) {
            if (fs::exists(p / "include" / "xi" / "xi.hpp")) {
                include_dir = (p / "include").string();
            }
            if (fs::exists(p / "plugins")) {
                plugins_dir = (p / "plugins").string();
            }
            if (!include_dir.empty() && !plugins_dir.empty()) break;
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
    }
    if (include_dir.empty()) {
        std::fprintf(stderr, "[runner] cannot find backend/include — run from repo tree or pass --script with absolute includes\n");
        return 2;
    }

    // Scan plugins: built-in + any --plugins-dir the user gave us.
    xi::PluginManager pm;
    int n = pm.scan_plugins(plugins_dir);
    std::fprintf(stderr, "[runner] scanned %d plugins from %s\n", n, plugins_dir.c_str());
    for (auto& d : args.extra_plugins) {
        int extra = pm.scan_plugins(d);
        std::fprintf(stderr, "[runner] scanned %d extra plugins from %s\n", extra, d.c_str());
    }

    // Install the trigger hook so any image source using emit_trigger still
    // works even without a live WS server. The bus's primary sink stays
    // null — trigger events just release their images and move on.
    auto host_api = xi::ImagePool::make_host_api();
    xi::install_trigger_hook(host_api);

    // Restore instances (plugins + configs) from project.json.
    if (!pm.open_project(args.project_dir)) {
        std::fprintf(stderr, "[runner] open_project failed for %s (missing project.json?)\n",
                     args.project_dir.c_str());
        return 2;
    }
    std::fprintf(stderr, "[runner] project loaded: %s\n", args.project_dir.c_str());

    // Compile the inspection script.
    std::string script_path = args.script_override.empty()
        ? (fs::path(args.project_dir) / "inspection.cpp").string()
        : args.script_override;
    if (!fs::exists(script_path)) {
        std::fprintf(stderr, "[runner] script not found: %s\n", script_path.c_str());
        return 2;
    }
    xi::script::CompileRequest req;
    req.source_path = script_path;
    req.output_dir  = (fs::temp_directory_path() / "xinsp2_runner_build").string();
    req.include_dir = include_dir;
    auto res = xi::script::compile(req);
    if (!res.ok) {
        std::fprintf(stderr, "[runner] compile failed:\n%s\n", res.build_log.c_str());
        return 1;
    }

    xi::script::LoadedScript script;
    std::string err;
    if (!xi::script::load_script(res.dll_path, script, err)) {
        std::fprintf(stderr, "[runner] %s\n", err.c_str());
        return 1;
    }
    if (script.set_use_callbacks) {
        script.set_use_callbacks(
            (void*)use_process_cb, (void*)use_exchange_cb,
            (void*)use_grab_cb, (void*)&host_api);
    }
    std::fprintf(stderr, "[runner] script loaded: %s\n", res.dll_path.c_str());

    // Run the frames. We build the JSON as a string then write it once to
    // avoid partial-file surprises if the process is killed mid-run.
    std::string proj_json;
    xi::proto::json_escape_into(proj_json, args.project_dir);
    std::string body;
    body.reserve(args.frames * 256);
    body += "{\"project\":";
    body += proj_json;
    body += ",\"frames\":[";

    int crashed = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < args.frames; ++i) {
        if (script.reset) script.reset();
        try {
            script.inspect(i);
        } catch (const seh_exception& e) {
            std::fprintf(stderr, "[runner] frame %d crashed: 0x%08X\n", i, e.code);
            ++crashed;
            continue;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[runner] frame %d threw: %s\n", i, e.what());
            ++crashed;
            continue;
        }
        std::vector<char> buf(256 * 1024);
        int vn = script.snapshot ? script.snapshot(buf.data(), (int)buf.size()) : 0;
        if (vn < 0) {
            buf.resize((size_t)(-vn) + 1024);
            vn = script.snapshot(buf.data(), (int)buf.size());
        }
        if (i > 0) body += ",";
        body += "{\"frame\":";
        body += std::to_string(i);
        body += ",\"vars\":";
        body += (vn > 0 ? std::string(buf.data(), (size_t)vn) : "[]");
        body += "}";
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    body += "],\"summary\":{";
    body += "\"frames_run\":" + std::to_string(args.frames);
    body += ",\"crashed\":"   + std::to_string(crashed);
    body += ",\"total_ms\":"  + std::to_string((int)ms);
    body += "}}";

    std::ofstream report(args.output);
    report << body;
    report.close();

    std::fprintf(stderr, "[runner] wrote %s — %d frames in %.0fms (%d crashed)\n",
                 args.output.c_str(), args.frames, ms, crashed);

    xi::script::unload_script(script);
    // Skip global dtors: plugin-DLL ordering during C++ static teardown is
    // fragile (plugin statics can outlive registries they reference and
    // fault at exit). The report is already on disk; let the OS reap the
    // process. Short-lived headless utility — no long-running state to flush.
    std::fflush(stderr);
    std::fflush(stdout);
    int code = crashed > 0 ? 1 : 0;
    ExitProcess((UINT)code);
    return code; // unreachable
}
