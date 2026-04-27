#pragma once
//
// xi_script_process_adapter.hpp — host-side handle to a user inspection
// script running in xinsp-script-runner.exe. Public surface mirrors the
// methods of LoadedScript that service_main currently calls inline, so
// service_main can pick the in-proc OR isolated path with the same
// dispatch shape.
//
// Phase 3.8 minimum: covers start / stop / ok / inspect / snapshot.
// set_param / set_state / list_instances and friends are deferred —
// they map to additional RPCs that follow the use_* pattern (one-line
// Session::call wrappers), so they're cheap to add when the call sites
// in service_main need them.
//

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#include "xi_ipc.hpp"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace xi::script {

class ScriptProcessAdapter {
public:
    ScriptProcessAdapter() = default;
    ScriptProcessAdapter(const ScriptProcessAdapter&) = delete;
    ScriptProcessAdapter& operator=(const ScriptProcessAdapter&) = delete;
    ~ScriptProcessAdapter() { stop(); }

    // Spawn xinsp-script-runner.exe with the given script DLL,
    // accept its pipe. The caller has already created the SHM region
    // and stored its name; the runner attaches to it on startup so
    // host_api->image_data resolves cross-process.
    //
    // Returns false on any failure; err carries a human message.
    bool start(const std::filesystem::path& runner_exe,
               const std::filesystem::path& script_dll,
               const std::string& shm_name,
               std::string& err) {
        stop();
        // Cache spawn args for try_respawn — same pattern as
        // ProcessInstanceAdapter Phase 2.7.
        runner_exe_path_ = runner_exe;
        script_dll_path_ = script_dll;
        shm_name_        = shm_name;
        return spawn_(err);
    }

private:
    // Internal: spin up the runner process + accept pipe + Session. Used
    // by start() AND try_respawn_locked_(). Caller must clear any prior
    // state (stop()) before calling.
    bool spawn_(std::string& err) {
        char nbuf[112];
        std::snprintf(nbuf, sizeof(nbuf), "xinsp2-script-pipe-%lu-r%d",
                      (unsigned long)GetCurrentProcessId(), respawn_count_);
        pipe_name_ = nbuf;
        std::string cmd = "\"" + runner_exe_path_.string() + "\""
            + " --pipe="  + pipe_name_
            + " --shm="   + shm_name_
            + " --script-dll=\"" + script_dll_path_.string() + "\"";

        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back(0);
        if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, nullptr, &si, &pi)) {
            err = "CreateProcess(xinsp-script-runner) failed: "
                + std::to_string(GetLastError());
            return false;
        }
        runner_proc_ = pi.hProcess;
        CloseHandle(pi.hThread);

        try {
            sess_ = std::make_unique<ipc::Session>(ipc::Pipe::accept_one(pipe_name_));
        } catch (const std::exception& e) {
            err = std::string("pipe accept failed: ") + e.what();
            TerminateProcess(runner_proc_, 1);
            CloseHandle(runner_proc_); runner_proc_ = nullptr;
            return false;
        }
        // Re-install handler on the new session so use_* callbacks
        // from the (re-loaded) script keep flowing back to the host.
        if (cached_handler_) sess_->set_handler(cached_handler_);
        return true;
    }
public:

    void stop() {
        if (sess_) {
            // Best-effort DESTROY; session destructor closes the pipe regardless.
            try { sess_->call(ipc::RPC_DESTROY, {}); } catch (...) {}
            sess_.reset();
        }
        if (runner_proc_) {
            DWORD wait = WaitForSingleObject(runner_proc_, 2000);
            if (wait != WAIT_OBJECT_0) TerminateProcess(runner_proc_, 1);
            CloseHandle(runner_proc_); runner_proc_ = nullptr;
        }
    }

    bool ok() const { return sess_ && sess_->valid(); }
    DWORD runner_pid() const {
        return runner_proc_ ? GetProcessId(runner_proc_) : 0;
    }

    // Install a session-handler that the runner can call back into.
    // service_main registers this so use_process / use_exchange /
    // use_grab requests from the script reach InstanceRegistry.
    // Cached so try_respawn re-installs it on the fresh session.
    void set_handler(ipc::Session::Handler h) {
        cached_handler_ = std::move(h);
        if (sess_) sess_->set_handler(cached_handler_);
    }

    int   respawn_count() const { return respawn_count_; }
    bool  is_dead()       const { return dead_; }

    // Run xi_inspect_entry(frame) inside the script and pull
    // ValueStore back as JSON. err set on RPC failure.
    //
    // If the runner has died since the last call (pipe broken on
    // write), we transparently respawn a fresh runner, restore the
    // handler, and retry once. Rate-limited the same way as
    // ProcessInstanceAdapter (3 respawns per 60s window).
    bool inspect_and_snapshot(int frame, std::string& vars_json, std::string& err) {
        if (!ok()) { err = "script not running"; return false; }
        try {
            return run_once_(frame, vars_json, err);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[ScriptProcessAdapter] RPC failed: %s — attempting respawn\n",
                e.what());
            if (!try_respawn_(err)) return false;
            // Retry the call on the fresh runner; if THIS throws we
            // surface the failure.
            try {
                return run_once_(frame, vars_json, err);
            } catch (const std::exception& e2) {
                err = e2.what();
                dead_ = true;
                return false;
            }
        }
    }

private:
    bool run_once_(int frame, std::string& vars_json, std::string& err) {
        ipc::Writer w; w.u32((uint32_t)frame);
        auto rsp = sess_->call(ipc::RPC_SCRIPT_RUN, w.buf());
        if (rsp.type == ipc::RPC_TYPE_ERROR) {
            err.assign((char*)rsp.payload.data(), rsp.payload.size());
            return false;
        }
        ipc::Reader r(rsp.payload);
        auto bytes = r.bytes();
        vars_json.assign((char*)bytes.data(), bytes.size());
        return true;
    }

    bool try_respawn_(std::string& err) {
        using clock = std::chrono::steady_clock;
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now().time_since_epoch()).count();
        if (now_ms - respawn_window_start_ms_ > 60000) {
            respawn_window_start_ms_ = now_ms;
            respawn_count_in_window_ = 0;
        }
        if (respawn_count_in_window_ >= 3) {
            err = "respawn cap hit (3/60s)";
            std::fprintf(stderr,
                "[ScriptProcessAdapter] respawn cap hit — staying dead\n");
            dead_ = true;
            return false;
        }
        ++respawn_count_in_window_;
        ++respawn_count_;

        // Tear down stale state.
        sess_.reset();
        if (runner_proc_) {
            TerminateProcess(runner_proc_, 1);
            CloseHandle(runner_proc_);
            runner_proc_ = nullptr;
        }

        if (!spawn_(err)) {
            std::fprintf(stderr,
                "[ScriptProcessAdapter] respawn spawn failed: %s\n",
                err.c_str());
            dead_ = true;
            return false;
        }
        std::fprintf(stderr,
            "[ScriptProcessAdapter] respawned — new runner pid=%lu (count=%d)\n",
            (unsigned long)GetProcessId(runner_proc_), respawn_count_);
        dead_ = false;
        return true;
    }

    std::string pipe_name_;
    HANDLE      runner_proc_ = nullptr;
    std::unique_ptr<ipc::Session> sess_;

    // Spawn parameters cached from start() so try_respawn doesn't need
    // them again.
    std::filesystem::path runner_exe_path_;
    std::filesystem::path script_dll_path_;
    std::string           shm_name_;

    // Auto-respawn rate-limit.
    int      respawn_count_           = 0;
    int      respawn_count_in_window_ = 0;
    int64_t  respawn_window_start_ms_ = 0;
    bool     dead_                    = false;

    // Re-installed on every respawned session.
    ipc::Session::Handler cached_handler_;
};

} // namespace xi::script
