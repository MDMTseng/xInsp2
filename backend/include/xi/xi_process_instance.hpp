#pragma once
//
// xi_process_instance.hpp — InstanceBase adapter that hosts a plugin in
// a separate `xinsp-worker.exe` process. Methods proxy across the pipe
// using xi_ipc; pixel data crosses via the shared SHM region — no copy.
//
// One worker process per isolated instance. Lifecycle:
//
//   ctor  → spawn worker → accept pipe → send CREATE
//   <method calls> → RPC over pipe
//   dtor  → send DESTROY → wait for worker → close handles
//
// Workers crash → adapter sees EOF on next RPC, marks itself dead,
// surfaces an error to the caller. The PluginManager owner is then
// expected to clean it up; auto-respawn is deferred to a later phase.
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

#include "xi_abi.h"
#include "xi_instance.hpp"
#include "xi_ipc.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace xi {

class ProcessInstanceAdapter : public InstanceBase {
public:
    // Spawn the worker, accept the pipe, run CREATE. Throws on any failure
    // — caller (PluginManager) catches + records as an open warning.
    ProcessInstanceAdapter(std::string instance_name,
                           std::string plugin_name,
                           const std::filesystem::path& worker_exe,
                           const std::filesystem::path& plugin_dll,
                           const std::string& shm_name)
        : name_(std::move(instance_name)), plugin_name_(std::move(plugin_name)),
          worker_exe_path_(worker_exe), plugin_dll_path_(plugin_dll),
          shm_name_(shm_name)
    {
        // Per-instance pipe name. Worker process is short-lived enough
        // that name reuse across runs isn't a concern.
        char nbuf[96];
        std::snprintf(nbuf, sizeof(nbuf), "xinsp2-pipe-%lu-%s",
                      (unsigned long)GetCurrentProcessId(), name_.c_str());
        pipe_name_ = nbuf;

        // Spawn the worker first; pipe accept blocks until it connects.
        std::string cmd = "\"" + worker_exe.string() + "\""
            + " --pipe=" + pipe_name_
            + " --shm=" + shm_name
            + " --plugin-dll=\"" + plugin_dll.string() + "\""
            + " --instance=" + name_;

        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back(0);
        if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, nullptr, &si, &pi)) {
            throw std::runtime_error("CreateProcess(xinsp-worker) failed: "
                + std::to_string(GetLastError()));
        }
        worker_proc_ = pi.hProcess;
        CloseHandle(pi.hThread);

        try {
            pipe_ = ipc::Pipe::accept_one(pipe_name_);
        } catch (...) {
            // Worker started but failed to connect — kill it before we throw.
            TerminateProcess(worker_proc_, 1);
            CloseHandle(worker_proc_); worker_proc_ = nullptr;
            throw;
        }

        // CREATE — gives the plugin its real instance name.
        ipc::Writer w; w.str(name_); w.str(plugin_dll.string());
        auto rsp = call_(ipc::RPC_CREATE, w.buf());
        if (rsp.type == ipc::RPC_TYPE_ERROR) {
            std::string msg(rsp.payload.begin(), rsp.payload.end());
            shutdown_();
            throw std::runtime_error("worker CREATE failed: " + msg);
        }
        std::fprintf(stderr,
            "[ProcessInstanceAdapter] '%s' spawned worker pid=%lu pipe=%s\n",
            name_.c_str(), (unsigned long)GetProcessId(worker_proc_),
            pipe_name_.c_str());
    }

    ~ProcessInstanceAdapter() override { shutdown_(); }

    // ---- InstanceBase ----
    const std::string& name() const override { return name_; }
    std::string plugin_name() const override { return plugin_name_; }

    std::string get_def() const override {
        // get_def is `const` on InstanceBase but RPC needs to mutate the
        // pipe state. const_cast is the smaller evil here than touching
        // the base interface.
        auto* self = const_cast<ProcessInstanceAdapter*>(this);
        if (self->dead_) return "{}";
        try {
            auto rsp = self->call_(ipc::RPC_GET_DEF, {});
            if (rsp.type == ipc::RPC_TYPE_ERROR) return "{}";
            ipc::Reader r(rsp.payload);
            auto bytes = r.bytes();
            return std::string(bytes.begin(), bytes.end());
        } catch (...) { self->dead_ = true; return "{}"; }
    }

    bool set_def(const std::string& json) override {
        if (dead_) return false;
        try {
            ipc::Writer w; w.str(json);
            auto rsp = call_(ipc::RPC_SET_DEF, w.buf());
            if (rsp.type == ipc::RPC_TYPE_ERROR) return false;
            ipc::Reader r(rsp.payload);
            bool ok = (r.u8() == 1);
            if (ok) {
                // Cache for auto-restore after a respawn so the worker
                // we get back isn't blank-slate.
                std::lock_guard<std::mutex> lk(mu_);
                saved_def_ = json;
            }
            return ok;
        } catch (...) { dead_ = true; return false; }
    }

    std::string exchange(const std::string& cmd_json) override {
        if (dead_) return "{}";
        try {
            ipc::Writer w; w.str(cmd_json);
            auto rsp = call_(ipc::RPC_EXCHANGE, w.buf());
            if (rsp.type == ipc::RPC_TYPE_ERROR) return "{}";
            ipc::Reader r(rsp.payload);
            auto bytes = r.bytes();
            return std::string(bytes.begin(), bytes.end());
        } catch (...) { dead_ = true; return "{}"; }
    }

    // ---- Process via RPC ----
    //
    // Fills `out` with the worker's response. SHM-backed handles, so the
    // pixel buffer is the SAME memory the script will read.
    //
    // The output handle's refcount is whatever the plugin set (1 from
    // shm_create_image). The caller owns that ref and is expected to
    // release it after consuming the result.
    bool process_via_rpc(const xi_record* in,
                         xi_record_out* out,
                         std::string* err = nullptr) {
        if (dead_) { if (err) *err = "worker dead"; return false; }
        // Note: image_count == 0 is legal (source plugins, json-only ops).
        // Pass handle 0 in that case; the worker side resolves to a null
        // input image and the plugin's process() sees an empty Record.
        try {
            ipc::Writer w;
            uint64_t in_h = (in && in->image_count > 0 && in->images)
                            ? in->images[0].handle
                            : 0ull;
            w.u64(in_h);
            const char* j = (in && in->json) ? in->json : "{}";
            w.bytes(j, std::strlen(j));
            auto rsp = call_(ipc::RPC_PROCESS, w.buf());
            if (rsp.type == ipc::RPC_TYPE_ERROR) {
                if (err) *err = std::string(rsp.payload.begin(), rsp.payload.end());
                return false;
            }
            ipc::Reader r(rsp.payload);
            uint64_t out_h = r.u64();
            auto out_key_bytes  = r.bytes();
            auto out_json_bytes = r.bytes();

            // Stash the response in instance-owned storage so the caller
            // can read it after this call returns. xi_record_out's
            // pointers must outlive the call site's stack — these
            // members do. Single-image output is the common case.
            out_key_.assign(out_key_bytes.begin(), out_key_bytes.end());
            out_image_.key = out_key_.c_str();
            out_image_.handle = out_h;
            out_json_.assign(out_json_bytes.begin(), out_json_bytes.end());

            out->images       = (out_h ? &out_image_ : nullptr);
            out->image_count  = (out_h ? 1 : 0);
            // xi_record_out::json is char* (plugin-owned, non-const ABI).
            // out_json_ outlives this call (member of the adapter), so
            // exposing data() is safe; nothing writes to it.
            out->json         = out_json_.data();
            return true;
        } catch (const std::exception& e) {
            if (err) *err = e.what();
            dead_ = true;
            return false;
        }
    }

    bool is_dead() const { return dead_; }
    int  respawn_count() const { return respawn_count_; }
    DWORD worker_pid() const {
        return worker_proc_ ? GetProcessId(worker_proc_) : 0;
    }

    // Maximum time a single RPC may block before the adapter cancels
    // the in-flight read/write via CancelIoEx and treats it like a
    // crashed worker. 30s is generous for normal plugin process()
    // calls; tune via set_call_timeout_ms() for slow operators (e.g.
    // long-exposure cameras' grab()).
    int  call_timeout_ms() const { return call_timeout_ms_; }
    void set_call_timeout_ms(int ms) { call_timeout_ms_ = ms; }

private:
    ipc::Frame call_(uint32_t type, const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lk(mu_);
        // First attempt. If pipe is broken (worker died since last
        // call), this throws — we catch, respawn, then retry once.
        try {
            return raw_call_locked_(type, payload);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[ProcessInstanceAdapter] '%s' RPC failed: %s — attempting respawn\n",
                name_.c_str(), e.what());
            if (!try_respawn_locked_()) throw;
            // Retry once on the freshly spawned worker. If THIS fails
            // we propagate so the caller sees the error.
            return raw_call_locked_(type, payload);
        }
    }

    ipc::Frame raw_call_locked_(uint32_t type, const std::vector<uint8_t>& payload) {
        // Watchdog thread: waits up to call_timeout_ms_ on a condvar.
        // If the call doesn't complete in time, it CancelIoEx's the
        // pipe handle, which makes the in-flight ReadFile / WriteFile
        // return with ERROR_OPERATION_ABORTED → Pipe::read_exact /
        // write_all throws → call_() catches and triggers respawn.
        std::mutex                      m;
        std::condition_variable         cv;
        bool                            done    = false;
        bool                            timeout = false;
        HANDLE                          ph      = pipe_.native_handle();
        const int                       wait_ms = call_timeout_ms_;
        std::thread watchdog([&] {
            std::unique_lock<std::mutex> lk(m);
            if (cv.wait_for(lk, std::chrono::milliseconds(wait_ms),
                            [&]{ return done; })) {
                return;          // RPC completed in time
            }
            timeout = true;
            // CancelIoEx works even for non-overlapped pending I/O
            // since Vista. Cancels ReadFile + WriteFile on this pipe
            // for ALL threads — fine, only one in flight at a time.
            CancelIoEx(ph, nullptr);
        });

        // Helper: signal watchdog + join.
        auto release_watchdog = [&] {
            { std::lock_guard<std::mutex> lk(m); done = true; }
            cv.notify_all();
            if (watchdog.joinable()) watchdog.join();
        };

        try {
            uint32_t seq = ++seq_;
            ipc::send_frame(pipe_, seq, type, payload.data(), (uint32_t)payload.size());
            auto f = ipc::recv_frame(pipe_);
            release_watchdog();
            if (f.seq != seq) throw std::runtime_error("RPC seq mismatch");
            return f;
        } catch (...) {
            release_watchdog();
            if (timeout) {
                // Re-throw with a clearer error so respawn logs see
                // "timeout" rather than "pipe read EOF".
                throw std::runtime_error(
                    "worker call timeout (" + std::to_string(wait_ms) + "ms)");
            }
            throw;
        }
    }

    // Reset pipe + worker proc, spawn a fresh worker, send CREATE, and
    // re-apply the most recent successful set_def so the new instance
    // isn't blank. Rate-limited: at most 3 respawns per 60s rolling
    // window, after which dead_ stays true and call_ throws.
    bool try_respawn_locked_() {
        using clock = std::chrono::steady_clock;
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now().time_since_epoch()).count();
        if (now_ms - respawn_window_start_ms_ > 60000) {
            respawn_window_start_ms_ = now_ms;
            respawn_count_in_window_ = 0;
        }
        if (respawn_count_in_window_ >= 3) {
            std::fprintf(stderr,
                "[ProcessInstanceAdapter] '%s' respawn cap hit (3/60s) — staying dead\n",
                name_.c_str());
            return false;
        }
        ++respawn_count_in_window_;
        ++respawn_count_;

        // Tear down stale state.
        pipe_ = ipc::Pipe{};
        if (worker_proc_) {
            TerminateProcess(worker_proc_, 1);
            CloseHandle(worker_proc_);
            worker_proc_ = nullptr;
        }

        // New unique pipe name so we don't collide with the dead one.
        char nbuf[112];
        std::snprintf(nbuf, sizeof(nbuf), "xinsp2-pipe-%lu-%s-r%d",
                      (unsigned long)GetCurrentProcessId(),
                      name_.c_str(), respawn_count_);
        pipe_name_ = nbuf;

        std::string cmd = "\"" + worker_exe_path_.string() + "\""
            + " --pipe=" + pipe_name_
            + " --shm=" + shm_name_
            + " --plugin-dll=\"" + plugin_dll_path_.string() + "\""
            + " --instance=" + name_;

        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back(0);
        if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, nullptr, &si, &pi)) {
            std::fprintf(stderr,
                "[ProcessInstanceAdapter] '%s' respawn CreateProcess failed (%lu)\n",
                name_.c_str(), GetLastError());
            return false;
        }
        worker_proc_ = pi.hProcess;
        CloseHandle(pi.hThread);

        try {
            pipe_ = ipc::Pipe::accept_one(pipe_name_);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[ProcessInstanceAdapter] '%s' respawn accept failed: %s\n",
                name_.c_str(), e.what());
            TerminateProcess(worker_proc_, 1);
            CloseHandle(worker_proc_); worker_proc_ = nullptr;
            return false;
        }

        // Re-CREATE on the new worker.
        seq_ = 0;
        ipc::Writer w; w.str(name_); w.str(plugin_dll_path_.string());
        try {
            auto rsp = raw_call_locked_(ipc::RPC_CREATE, w.buf());
            if (rsp.type == ipc::RPC_TYPE_ERROR) {
                std::string msg(rsp.payload.begin(), rsp.payload.end());
                std::fprintf(stderr,
                    "[ProcessInstanceAdapter] '%s' respawn CREATE error: %s\n",
                    name_.c_str(), msg.c_str());
                return false;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[ProcessInstanceAdapter] '%s' respawn CREATE threw: %s\n",
                name_.c_str(), e.what());
            return false;
        }

        // Restore the last known-good config so the new instance isn't
        // back at default. Best-effort — failure to set_def is logged
        // but doesn't block the respawn from being usable.
        if (!saved_def_.empty() && saved_def_ != "{}") {
            ipc::Writer sw; sw.str(saved_def_);
            try { raw_call_locked_(ipc::RPC_SET_DEF, sw.buf()); }
            catch (...) {
                std::fprintf(stderr,
                    "[ProcessInstanceAdapter] '%s' respawn restore-def failed\n",
                    name_.c_str());
            }
        }

        dead_ = false;
        std::fprintf(stderr,
            "[ProcessInstanceAdapter] '%s' respawned — new worker pid=%lu (count=%d)\n",
            name_.c_str(), (unsigned long)GetProcessId(worker_proc_),
            respawn_count_);
        return true;
    }

    void shutdown_() {
        // Best-effort DESTROY then drop the pipe. Worker exits on either.
        if (!dead_) {
            try { call_(ipc::RPC_DESTROY, {}); } catch (...) {}
            dead_ = true;
        }
        pipe_ = ipc::Pipe{};   // close pipe handle
        if (worker_proc_) {
            // 2 s grace, then nuke. Plugin destructor might be slow;
            // we don't want to wait forever for a stuck worker.
            DWORD wait = WaitForSingleObject(worker_proc_, 2000);
            if (wait != WAIT_OBJECT_0) TerminateProcess(worker_proc_, 1);
            CloseHandle(worker_proc_);
            worker_proc_ = nullptr;
        }
    }

    std::string name_;
    std::string plugin_name_;
    std::string pipe_name_;
    HANDLE      worker_proc_ = nullptr;
    ipc::Pipe   pipe_;
    mutable std::mutex mu_;
    uint32_t    seq_ = 0;
    std::atomic<bool> dead_{false};

    // Spawn parameters — cached so try_respawn_locked_ can re-spawn
    // identical workers without the caller having to pass them again.
    std::filesystem::path worker_exe_path_;
    std::filesystem::path plugin_dll_path_;
    std::string           shm_name_;

    // Auto-respawn rate-limit: at most 3 respawns per 60s rolling window.
    int      respawn_count_           = 0;   // total this lifetime, for diagnostics
    int      respawn_count_in_window_ = 0;
    int64_t  respawn_window_start_ms_ = 0;

    // Per-call hard timeout. A worker that hangs (plugin's process()
    // deadlocks, infinite loop, etc.) gets cancelled at this point and
    // treated like a crash — respawn_locked_ kicks in.
    int      call_timeout_ms_         = 30000;

    // Most recent set_def value that the worker accepted. Re-applied
    // automatically on respawn so isolation:process plugins keep their
    // tuning across crashes.
    std::string saved_def_;

    // Storage for the most recent process_via_rpc reply — pointers in
    // xi_record_out alias these.
    xi_record_image out_image_{};
    std::string     out_key_;
    std::string     out_json_;
};

} // namespace xi
