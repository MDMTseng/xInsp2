//
// runner_main.cpp — xinsp-runner.exe
//
// Headless production runner. Takes a project folder, compiles the
// inspection script if needed, restores all instances, and runs
// inspect() N times.
//
// No WebSocket. No GUI. No plugins beyond what the project uses. The
// smallest possible binary that turns "saved project" → "execution log".
//
// What the report captures: both EXECUTION status AND per-frame inspection
// VERDICT. The result callback (xi::result(code,msg) → xi_script_set_result_callback)
// is wired below, so each frame records its (code, class, msg): the verdict
// class is derived from the signed code — code>0 "ok", code in -1..-989999
// "ng", code==0 "na", a caught crash "crashed", a frame that ran but set no
// result "no_verdict". The summary carries the requested frame count
// (frames_run), the crash count (crashed), total wall time (total_ms), AND an
// additive per-class tally: counts{ok,ng,na,no_verdict,crashed}.
//
// Health (schema xi.health/1): the runner drives the core-owned health registry
// through its lifecycle (project_loaded -> script ok/failed -> running -> degraded
// on the first caught inspect crash) and logs each health_changed to the execution
// log. summary.health carries the FINAL state ({schema,state,since_ms,last_reason}),
// so a crashed-then-degraded run is visible in the artifact without replaying the
// per-frame log. See docs/new_gen/04-health-contract.md.
//
// Identity envelope (ADDITIVE — mirrors the live backend's run_result): the
// report now carries the SAME traceability identity the service stamps on
// `run_result`, so a headless run is traceable back to a process + frame.
// Top-level: schema="xi.run-outcome/1", boot_id (a random 128-bit value
// generated ONCE at runner startup, formatted as 32-char lowercase hex, stable
// for the whole run), and station_id (optional, from env XINSP_STATION_ID,
// omitted when empty). Per frame: run_id (the frame index) and inspection_id =
// "<station_id>/<boot_id>/<run_id>" (station_id may be empty → leading "/").
// boot_id/inspection_id are formatted byte-for-byte like service_main.cpp's
// init_process_identity_ / emit_run_result, so headless and live reports agree.
//
// IMPORTANT — exit code is still an EXECUTION status, NOT a verdict roll-up.
// Exit 0 means "every frame dispatched without crashing"; an inspection NG
// does NOT change the exit code. Infra/compile/load failures and script
// crashes drive the exit code; grep the report's per-frame "class" / summary
// "counts" for pass/fail. (VAR value-tracking lives in the `expose` plugin.)
//
// Usage:
//   xinsp-runner.exe <project-folder> [--frames=N] [--output=report.json]
//                                     [--script=path.cpp] [--plugins-dir=...]
//
// Example:
//   xinsp-runner.exe C:\factory\project --frames=1000 --output=today.json
//
// Exit: 0 if all frames dispatched without crashing; 1 on compile/load
// failure or any script crash; 2 on infra failure (bad args, missing
// project/script, or the final report could not be written — the report is
// written atomically and checked, so exit 0 guarantees a complete artifact).
// This is an EXECUTION status, not an inspection-verdict roll-up — grep the
// per-frame "class" or summary "counts" for pass/fail (see the verdict note
// above).
//

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <xi/xi_abi.hpp>
#include <xi/xi_atomic_io.hpp>   // xi::atomic_write — temp+rename report write; no torn/missing artifact behind exit 0
#include <xi/xi_certify.hpp>     // Part III G1: --certify-plugin child mode + verdict subprocess
#include <xi/xi_crash_dump.hpp>  // xi::crash::install() — a crashed certify still yields a minidump
#include <xi/xi_health.hpp>      // xi::health() — canonical health/state contract (schema xi.health/1)
#include <xi/xi_image.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_instance.hpp>
#include <xi/xi_plugin_manager.hpp>
#include <xi/xi_protocol.hpp>
#include <xi/xi_script_compiler.hpp>
#include <xi/xi_script_loader.hpp>
#include <xi/xi_trigger_bus.hpp>

#include "xi_result_class.hpp"   // XI_SYS_* band + outcome_class_for_code + kRunResultSchema (shared with the service)

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <eh.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// --- SEH translator so backend survives script crashes ------------------
#include <xi/xi_seh.hpp>
using xi::seh_exception;
#ifdef _MSC_VER
using xi::seh_translator;   // MSVC-only; the portable installer is xi::install_seh_translator()
#endif

// --- xi::use() callbacks (minimal copy of service_main equivalents) -----

// THE CUT: the Record process bridge (use_process_cb — built an xi_record, took the
// borrowed-doc / JSON-serialize path into adapter->process_fn()) is DELETED. The
// plugin data plane is the xi.pack@1 door only (use_pack_process_cb below); the
// process_fn ABI slot is fed nullptr in set_use_callbacks.

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

// polaris2 Gate P2 — minimal runner copy of the service's use_pack_process_cb:
// drive the target's xi.pack@1 pack door for xi::use(...).process(ScriptPack).
// Same return codes (0 ok / -1 miss / -2 crash / -4 no door / -5 sink target,
// U3 docs/new_gen/17); the runner has no item-14 quarantine machinery, so -3
// never occurs here, and it never marks adapters as sinks, so -5 is carried
// for contract symmetry only.
static int use_pack_process_cb(const char* name, xi_pack_handle in, xi_pack_handle* out) {
    if (out) *out = XI_PACK_NULL;
    if (!name || !out) return -1;
    auto inst = xi::InstanceRegistry::instance().find(name);
    if (!inst) return -1;
    auto* adapter = dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get());
    if (!adapter || !adapter->has_pack_door()) return -4;
    if (adapter->is_sink()) return -5;   // U3: sink feed is push(), fail-loud
    try {
        *out = adapter->run_pack_door(in);
        return 0;
    } catch (...) {
        std::fprintf(stderr, "[runner] use_pack_process('%s') crashed/threw\n", name);
        *out = XI_PACK_NULL;
        return -2;
    }
}

// --- per-frame verdict capture (mirror of service_main's g_run_result) ---
//
// The script calls xi::result(code,msg) at most once per inspect; that routes
// through result_cb below into g_run_result. Reset at the top of each frame so
// a frame that sets no result is distinguishable (set==false → "no_verdict").
// thread_local for symmetry with the backend, though the runner is single-lane.
struct RunResult { int code = 0; std::string msg; bool set = false; };
static thread_local RunResult g_run_result;

// Framework reserved band + code→class mapping + reserved-band rejection:
// SHARED with the live backend via xi_result_class.hpp (included above) —
// the former hand-mirrored copy is gone, so runner and service can't drift.
// (The runner never emits XI_SYS_DROPPED; sharing the full mapping is harmless.)

static void result_cb(int code, const char* msg) {
    // Host is the trust boundary: reserved-band codes are recorded as NA (0)
    // with the offending code preserved in the message (shared rejection core).
    if (!reject_reserved_result_code(code, msg, g_run_result.code, g_run_result.msg)) {
        g_run_result.code = code;
        g_run_result.msg.assign(msg ? msg : "");
    }
    g_run_result.set = true;
}

// --- process identity (mirror of service_main's run_result envelope) -----
//
// The runner drives script.inspect(i) directly and never goes through the
// backend's emit_run_result path, so it must GENERATE its own identity the same
// way the live backend does. schema/boot_id/inspection_id are formatted
// byte-for-byte like service_main.cpp (init_process_identity_ + emit_run_result)
// so a headless report and a live run_result agree. kRunResultSchema comes from
// the shared xi_result_class.hpp.

// Format a 128-bit value as a 32-char lowercase hex string ("hi" then "lo",
// each zero-padded to 16). Same encoding as service_main::trigger_id_hex.
static std::string id_hex_128(uint64_t hi, uint64_t lo) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (int shift = 60; shift >= 0; shift -= 4) s.push_back(d[(hi >> shift) & 0xF]);
    for (int shift = 60; shift >= 0; shift -= 4) s.push_back(d[(lo >> shift) & 0xF]);
    return s;
}

// Generate the per-process boot_id ONCE at startup (random 128-bit → 32-char
// lowercase hex). std::random_device + a seeded 64-bit engine, run once, mirrors
// service_main::init_process_identity_.
static std::string make_boot_id() {
    std::random_device rd;
    std::seed_seq seed{ rd(), rd(), rd(), rd(),
                        (unsigned)std::chrono::steady_clock::now().time_since_epoch().count() };
    std::mt19937_64 eng(seed);
    uint64_t hi = eng(), lo = eng();
    if (hi == 0 && lo == 0) lo = 1;   // never all-zero (would read as "null")
    return id_hex_128(hi, lo);
}

// --- args ---------------------------------------------------------------

struct Args {
    std::string project_dir;
    std::string output = "report.json";
    std::string script_override;       // --script=path; else project/inspect.cpp
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
        "  --script=PATH      override inspection source (default: <proj>/inspect.cpp)\n"
        "  --plugins-dir=DIR  extra plugin folder (repeatable)\n"
        "  -h, --help         show this help\n");
}

// --- main ---------------------------------------------------------------

static std::string get_exe_path() {
    char buf[MAX_PATH]; DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return (n == 0) ? std::string{} : std::string(buf, n);
}

static std::string get_exe_dir() {
    std::string s = get_exe_path();
    if (s.empty()) return {};
    auto slash = s.find_last_of("\\/");
    return (slash == std::string::npos) ? "." : s.substr(0, slash);
}

int main(int argc, char** argv) {
    // Part III G1.1 — certify mode: load a plugin DLL + call its factory once in
    // THIS throwaway child process, then exit with a verdict code (0 ok / 42
    // abi_mismatch / abnormal = crashed). Crash-isolation for discovery: a
    // malformed DLL faults HERE, never in the scanning backend. Handled first,
    // before the SEH translator install below, so a fault reaches the minidump
    // filter (xi::crash::install) rather than being translated + swallowed.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--certify-plugin") {
            const char* dir = (i + 1 < argc) ? argv[i + 1] : nullptr;
            xi::crash::install_minidump_writer();   // Breakpad when built in
            xi::crash::install();   // a crashed certify still yields a minidump
            int code = dir ? xi::certify::certify_in_process(dir)
                           : xi::certify::kExitAbiMismatch;
            std::fflush(stderr);
            std::fflush(stdout);
            return code;
        }
    }
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--version" || a == "-v") {
            std::printf("xinsp-runner %s (%s)\n", XINSP2_VERSION, XINSP2_COMMIT);
            return 0;
        }
    }
    Args args = parse_args(argc, argv);
    if (args.help || args.project_dir.empty()) {
        print_usage();
        return args.help ? 0 : 2;
    }
    // --frames is operator input parsed with stoi and never range-checked: a
    // negative value would wrap in the size_t reserve arithmetic below
    // (frames * 256 → ~1.8e19) and terminate with an uncaught
    // std::length_error before frame 0 — no report, and an exit outside the
    // documented contract. Reject it here as a bad arg (infra failure, exit
    // 2), same as a missing project folder / include dir.
    if (args.frames < 0) {
        std::fprintf(stderr, "[runner] bad --frames=%d — must be >= 0\n", args.frames);
        return 2;
    }
    if (!fs::exists(args.project_dir)) {
        std::fprintf(stderr, "[runner] project folder not found: %s\n", args.project_dir.c_str());
        return 2;
    }

    // Per-process identity for the report's run_result envelope (boot_id +
    // optional station_id). Generated ONCE, early, mirroring the backend's
    // init_process_identity_. boot_id is stable for the whole run; inspection_id
    // (formed per frame below) is the only part that varies.
    const std::string boot_id = make_boot_id();
    std::string station_id;
    if (const char* s = std::getenv("XINSP_STATION_ID"); s && *s) station_id = s;

    // Health/state contract (docs/new_gen/04-health-contract.md). The runner drives
    // the BE in-process — no service layer, no WS — so it drives the SAME health
    // registry the live backend does and logs every transition to its execution
    // log. A crashed-then-degraded run is then visible in BOTH the log (each
    // health_changed line) and the report summary (the final state), without
    // replaying frames. Lifecycle-only: the per-frame path never touches health.
    xi::health().set_notifier([](const std::string& ev) {
        std::fprintf(stderr, "[runner] health_changed %s\n", ev.c_str());
    });

    xi::install_seh_translator();   // MSVC: SEH→C++ translator; POSIX: no-op

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
    // G1.3 — certify each discovered plugin in a throwaway child (this same exe,
    // --certify-plugin mode) before arming it. A DLL that crashes certification
    // is skipped, surfaced via pm.certify_warnings(), and never loaded here.
    pm.set_certify_exe(get_exe_path());
    // QuiesceToken: the headless runner has no dispatch pool at all — nothing
    // to quiesce (see xi_quiesce_token.hpp).
    int n = pm.scan_plugins(xi::QuiesceToken::assert_no_dispatch(), plugins_dir);
    std::fprintf(stderr, "[runner] scanned %d plugins from %s\n", n, plugins_dir.c_str());
    for (auto& d : args.extra_plugins) {
        int extra = pm.scan_plugins(xi::QuiesceToken::assert_no_dispatch(), d);
        std::fprintf(stderr, "[runner] scanned %d extra plugins from %s\n", extra, d.c_str());
    }

    // The script host_api over the backend's ImagePool. THE CUT: install_trigger_hook
    // (the Record emit_record/emit_trigger wiring, deleted from xi_trigger_bus.hpp) is
    // no longer installed — the data plane is the xi.pack@1 door.
    auto host_api = xi::ImagePool::make_host_api();

    // Restore instances (plugins + configs) from project.json.
    if (!pm.open_project(xi::QuiesceToken::assert_no_dispatch(), args.project_dir)) {
        std::fprintf(stderr, "[runner] open_project failed for %s (missing project.json?)\n",
                     args.project_dir.c_str());
        return 2;
    }
    std::fprintf(stderr, "[runner] project loaded: %s\n", args.project_dir.c_str());
    xi::health().set_state(xi::SysState::ProjectLoaded);   // boot -> project_loaded

    // Compile the inspection script.
    std::string script_path = args.script_override.empty()
        ? (fs::path(args.project_dir) / "inspect.cpp").string()
        : args.script_override;
    if (!fs::exists(script_path)) {
        std::fprintf(stderr, "[runner] script not found: %s\n", script_path.c_str());
        return 2;
    }
    xi::script::CompileRequest req;
    req.source_path    = script_path;
    // J1 (RT5): per-PID — two runner processes would otherwise collide on the same
    // fixed xinsp2_runner_build (same DLL stem + shared PCH). Reap dead runners'
    // dirs first so the per-PID dirs don't accumulate and fill the disk.
    fs::path _runner_base = fs::temp_directory_path() / "xinsp2_runner_build";
    xi::script::reap_stale_build_dirs(_runner_base);
    req.output_dir     = (_runner_base / std::to_string(GetCurrentProcessId())).string();
    req.include_dir    = include_dir;
    req.opencv_dir     = xi::script::detail::probe_opencv_dir();
    req.turbojpeg_root = xi::script::detail::probe_turbojpeg_root();
    req.ipp_root       = xi::script::detail::probe_ipp_root();
    const std::string script_name = fs::path(script_path).filename().string();
    auto res = xi::script::compile(req);
    if (!res.ok) {
        std::fprintf(stderr, "[runner] compile failed:\n%s\n", res.build_log.c_str());
        // The script cannot be brought into service — record it failed so the health
        // transition (script -> failed / compile_error) is in the log before we bail.
        xi::health().set_script(xi::CompHealth::Failed, xi::kReasonCompileError, script_name);
        return 1;
    }

    xi::script::LoadedScript script;
    std::string err;
    if (!xi::script::load_script(res.dll_path, script, err)) {
        std::fprintf(stderr, "[runner] %s\n", err.c_str());
        xi::health().set_script(xi::CompHealth::Failed, xi::kReasonCompileError, script_name);
        return 1;
    }
    if (script.set_use_callbacks) {
        // THE CUT: process_fn slot is nullptr (the Record process bridge is gone);
        // the pack door is wired via set_use_pack_callback below. grab_fn is
        // nullptr too (SDK discards it); both params are retained in the ABI
        // signature.
        script.set_use_callbacks(
            nullptr, (void*)use_exchange_cb,
            nullptr, (void*)&host_api);
    }
    // polaris2 Gate P2: pack-door chaining (xi::use(name).process(pack)).
    // Optional symbol — older scripts don't export it.
    if (script.set_use_pack_callback) {
        script.set_use_pack_callback((void*)use_pack_process_cb);
    }
    // Wire the result callback so xi::result(code,msg) is captured per frame
    // (mirrors the backend's install in service_main). Optional symbol: scripts
    // that don't include xi_result.hpp leave this null → every frame is
    // "no_verdict", which is the honest outcome for a script that never verdicts.
    if (script.set_result_callback) {
        script.set_result_callback((void*)result_cb);
    }
    std::fprintf(stderr, "[runner] script loaded: %s\n", res.dll_path.c_str());
    xi::health().set_script(xi::CompHealth::Ok, "", script_name);   // script in service

    // Run the frames. We build the JSON as a string then write it once to
    // avoid partial-file surprises if the process is killed mid-run.
    std::string proj_json;
    xi::proto::json_escape_into(proj_json, args.project_dir);
    std::string body;
    // The reserve is a size HINT derived from operator input — cap the
    // arithmetic so a huge (but valid) --frames can't make reserve() throw
    // std::length_error or grab gigabytes up front. Past the cap the string
    // simply grows on demand. (args.frames >= 0 is guaranteed by the arg
    // check at startup.)
    constexpr int kReserveFramesCap = 1 << 20;   // 1M frames → 256 MB hint max
    const size_t reserve_frames = (args.frames < kReserveFramesCap)
                                      ? (size_t)args.frames
                                      : (size_t)kReserveFramesCap;
    body.reserve(reserve_frames * 256);
    body += "{\"project\":";
    body += proj_json;
    // Identity envelope (additive; mirrors the live backend's run_result).
    body += ",\"schema\":";
    xi::proto::json_escape_into(body, kRunResultSchema);
    body += ",\"boot_id\":";
    xi::proto::json_escape_into(body, boot_id);
    if (!station_id.empty()) {   // omit when unset, matching the backend wire
        body += ",\"station_id\":";
        xi::proto::json_escape_into(body, station_id);
    }
    body += ",\"frames\":[";

    int crashed = 0;
    // Additive per-class verdict tally (see outcome_class_for_code).
    int c_ok = 0, c_ng = 0, c_na = 0, c_no_verdict = 0, c_crashed = 0;
    // Emit one frame object:
    //   {"frame":i,"run_id":i,"inspection_id":"<station>/<boot>/<i>",
    //    "code":c,"class":"..","msg":".."}.
    // run_id is the frame index; inspection_id = "<station_id>/<boot_id>/<run_id>"
    // (station_id may be empty → leading "/"), matching emit_run_result exactly.
    auto emit_frame = [&](int i, int code, const char* cls, const std::string& msg) {
        if (i > 0) body += ",";
        body += "{\"frame\":";
        body += std::to_string(i);
        body += ",\"run_id\":";
        body += std::to_string(i);
        body += ",\"inspection_id\":";
        xi::proto::json_escape_into(body, station_id + "/" + boot_id + "/" + std::to_string(i));
        body += ",\"code\":";
        body += std::to_string(code);
        body += ",\"class\":";
        xi::proto::json_escape_into(body, cls);
        body += ",\"msg\":";
        xi::proto::json_escape_into(body, msg);
        body += "}";
    };
    // The run is live now. Continuous inspection → running (project_loaded ->
    // running). A caught inspect crash below flips this to degraded (once), so the
    // final summary state distinguishes a clean run from a crashed-then-degraded one.
    xi::health().set_state(xi::SysState::Running);
    bool health_degraded = false;   // flip running -> degraded on the first crash
    // A caught inspect crash is a runtime fault of the pipeline: mark the run
    // degraded (idempotent — later crashes coalesce). The per-frame `crashed`
    // counter carries the count; this carries the health transition.
    auto mark_run_degraded = [&] {
        if (!health_degraded) { xi::health().set_state(xi::SysState::Degraded); health_degraded = true; }
    };
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < args.frames; ++i) {
        if (script.reset) script.reset();
        // Reset the per-frame verdict BEFORE inspect so a frame that never calls
        // xi::result() is recorded as no_verdict, not the previous frame's value.
        g_run_result = RunResult{};
        try {
            script.inspect(i);
        } catch (const seh_exception& e) {
            std::fprintf(stderr, "[runner] frame %d crashed: 0x%08X\n", i, e.code);
            ++crashed;
            ++c_crashed;
            mark_run_degraded();
            char buf[64];
            std::snprintf(buf, sizeof buf, "inspect crashed: 0x%08X", e.code);
            emit_frame(i, XI_SYS_CRASHED, "crashed", buf);
            // The frame loop continues on THIS thread; after a STACK_OVERFLOW restore
            // the guard page (or hard-exit) before running the next frame's inspect.
            xi::recover_seh_stack_or_die(e.code, "runner frame loop");
            continue;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[runner] frame %d threw: %s\n", i, e.what());
            ++crashed;
            ++c_crashed;
            mark_run_degraded();
            emit_frame(i, XI_SYS_CRASHED, "crashed",
                       std::string("inspect threw: ") + e.what());
            continue;
        }
        // Frame ran to completion — derive its verdict from the captured result.
        // A frame that set no result is "no_verdict" (distinct from na=0). VAR
        // value-tracking is the `expose` plugin's concern; result()/state live on
        // their own paths.
        int code = g_run_result.set ? g_run_result.code : XI_SYS_NO_VERDICT;
        const char* cls = outcome_class_for_code(code);
        if      (std::strcmp(cls, "ok") == 0)         ++c_ok;
        else if (std::strcmp(cls, "ng") == 0)         ++c_ng;
        else if (std::strcmp(cls, "no_verdict") == 0) ++c_no_verdict;
        else                                          ++c_na;
        emit_frame(i, code, cls, g_run_result.msg);
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    body += "],\"summary\":{";
    body += "\"frames_run\":" + std::to_string(args.frames);
    body += ",\"crashed\":"   + std::to_string(crashed);
    body += ",\"total_ms\":"  + std::to_string((int)ms);
    body += ",\"counts\":{";
    body += "\"ok\":"          + std::to_string(c_ok);
    body += ",\"ng\":"         + std::to_string(c_ng);
    body += ",\"na\":"         + std::to_string(c_na);
    body += ",\"no_verdict\":" + std::to_string(c_no_verdict);
    body += ",\"crashed\":"    + std::to_string(c_crashed);
    body += "}";
    // Final health state (schema xi.health/1 top-level): "running" for a clean run,
    // "degraded" if any frame crashed, "project_loaded" if 0 frames ran. Lets a
    // consumer read the run's health verdict from the artifact without scanning the
    // per-frame log. last_reason is the reason that drove the last non-ok state ("" here).
    {
        std::string hstate, hreason; int64_t hsince = 0;
        xi::health().mirror_snapshot(hstate, hsince, hreason);
        body += ",\"health\":{\"schema\":";
        xi::proto::json_escape_into(body, xi::kHealthSchema);
        body += ",\"state\":";
        xi::proto::json_escape_into(body, hstate);
        body += ",\"since_ms\":" + std::to_string(hsince);
        body += ",\"last_reason\":";
        xi::proto::json_escape_into(body, hreason);
        body += "}";
    }
    body += "}}";

    // Atomic + checked: a bad --output path or disk-full mid-write must not
    // leave a missing/torn report behind an exit-0 "wrote" line (CI/MES reads
    // exit 0 = success). atomic_write stages to <output>.tmp and renames, so
    // consumers never observe a partial file; on failure the report is the
    // runner's whole artifact, so it exits 2 (infra failure, like the other
    // setup/load failures above).
    const bool report_ok = xi::atomic_write(args.output, body);
    if (report_ok) {
        std::fprintf(stderr, "[runner] wrote %s — %d frames in %.0fms (%d crashed)\n",
                     args.output.c_str(), args.frames, ms, crashed);
    } else {
        std::fprintf(stderr, "[runner] ERROR: failed to write report %s (bad path or disk full?) — %d frames in %.0fms (%d crashed)\n",
                     args.output.c_str(), args.frames, ms, crashed);
    }

    xi::script::unload_script(script);
    // Skip global dtors: plugin-DLL ordering during C++ static teardown is
    // fragile (plugin statics can outlive registries they reference and
    // fault at exit). The report is already on disk; let the OS reap the
    // process. Short-lived headless utility — no long-running state to flush.
    std::fflush(stderr);
    std::fflush(stdout);
    int code = !report_ok ? 2 : (crashed > 0 ? 1 : 0);
    // Skip C++ static dtors (see comment above): _Exit terminates without running
    // them, on both platforms (the Win32 ExitProcess equivalent).
    std::_Exit(code);
    return code; // unreachable
}
