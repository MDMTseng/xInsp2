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
        char nbuf[96];
        std::snprintf(nbuf, sizeof(nbuf), "xinsp2-script-pipe-%lu",
                      (unsigned long)GetCurrentProcessId());
        pipe_name_ = nbuf;
        std::string cmd = "\"" + runner_exe.string() + "\""
            + " --pipe="  + pipe_name_
            + " --shm="   + shm_name
            + " --script-dll=\"" + script_dll.string() + "\"";

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
        return true;
    }

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
    void set_handler(ipc::Session::Handler h) {
        if (sess_) sess_->set_handler(std::move(h));
    }

    // Run xi_inspect_entry(frame) inside the script and pull
    // ValueStore back as JSON. err set on RPC failure.
    bool inspect_and_snapshot(int frame, std::string& vars_json, std::string& err) {
        if (!ok()) { err = "script not running"; return false; }
        try {
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
        } catch (const std::exception& e) {
            err = e.what();
            return false;
        }
    }

private:
    std::string pipe_name_;
    HANDLE      runner_proc_ = nullptr;
    std::unique_ptr<ipc::Session> sess_;
};

} // namespace xi::script
