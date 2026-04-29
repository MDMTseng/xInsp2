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
#include "xi_trigger_bus.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace xi {

class ProcessInstanceAdapter : public InstanceBase {
public:
    // Spawn the worker, accept the pipe, run CREATE. Throws on any failure
    // — caller (PluginManager) catches + records as an open warning.
    ProcessInstanceAdapter(std::string instance_name,
                           std::string plugin_name,
                           const std::filesystem::path& worker_exe,
                           const std::filesystem::path& plugin_dll,
                           const std::string& shm_name,
                           std::string instance_folder = {})
        : name_(std::move(instance_name)), plugin_name_(std::move(plugin_name)),
          worker_exe_path_(worker_exe), plugin_dll_path_(plugin_dll),
          shm_name_(shm_name), instance_folder_(std::move(instance_folder))
    {
        // Per-instance pipe name. Worker process is short-lived enough
        // that name reuse across runs isn't a concern.
        char nbuf[96];
        std::snprintf(nbuf, sizeof(nbuf), "xinsp2-pipe-%lu-%s",
                      (unsigned long)GetCurrentProcessId(), name_.c_str());
        pipe_name_ = nbuf;

        // Spawn the worker first; pipe accept blocks until it connects.
        // `--instance-folder` is the persistent dir for this instance
        // (project/instances/<name>/) — worker registers it in its own
        // InstanceFolderRegistry so plugin code can call host->
        // instance_folder() / Plugin::folder_path() the same way it
        // would in-proc.
        std::string cmd = "\"" + worker_exe.string() + "\""
            + " --pipe=" + pipe_name_
            + " --shm=" + shm_name
            + " --plugin-dll=\"" + plugin_dll.string() + "\""
            + " --instance=" + name_;
        if (!instance_folder_.empty()) {
            cmd += " --instance-folder=\"" + instance_folder_ + "\"";
        }

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
        // Always-on reader thread. The reader owns ALL reads on the
        // pipe — writes still go through the per-call write path under
        // mu_. seq=0 frames (RPC_EMIT_TRIGGER) dispatch to the trigger
        // bus immediately; seq!=0 frames fulfil the matching in-flight
        // promise registered by do_call_locked_(). See the comment
        // block above run_reader_() for the broken-pipe history that
        // motivated this design.
        start_reader_();

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
        // Wire format (matches worker_main.cpp RPC_PROCESS handler):
        //   u32 in_count
        //   for each: str key | u64 handle
        //   bytes input_json
        // Reply:
        //   u32 out_count
        //   for each: str key | u64 handle
        //   bytes output_json
        //
        // Empty input (image_count == 0) is legal — source plugins
        // receive a json-only Record. Empty output is also legal —
        // sink plugins return only json.
        try {
            ipc::Writer w;
            uint32_t n_in = (in && in->images) ? (uint32_t)in->image_count : 0;
            w.u32(n_in);
            for (uint32_t i = 0; i < n_in; ++i) {
                const auto& img = in->images[i];
                w.str(img.key ? std::string(img.key) : std::string{});
                w.u64(img.handle);
            }
            const char* j = (in && in->json) ? in->json : "{}";
            w.bytes(j, std::strlen(j));
            auto rsp = call_(ipc::RPC_PROCESS, w.buf());
            if (rsp.type == ipc::RPC_TYPE_ERROR) {
                if (err) *err = std::string(rsp.payload.begin(), rsp.payload.end());
                return false;
            }
            ipc::Reader r(rsp.payload);
            uint32_t n_out = r.u32();
            // Reserve up front — xi_record_image::key borrows from
            // out_keys_[i].c_str(), and a vector realloc would
            // invalidate those pointers between the loop and the
            // moment the caller reads them.
            out_keys_.clear();
            out_keys_.reserve(n_out);
            out_images_.clear();
            out_images_.reserve(n_out);
            for (uint32_t i = 0; i < n_out; ++i) {
                auto k_bytes = r.bytes();
                uint64_t h   = r.u64();
                out_keys_.emplace_back(k_bytes.begin(), k_bytes.end());
                xi_record_image rec{};
                rec.key    = out_keys_.back().c_str();
                rec.handle = h;
                out_images_.push_back(rec);
            }
            auto out_json_bytes = r.bytes();
            out_json_.assign(out_json_bytes.begin(), out_json_bytes.end());

            out->images      = out_images_.empty() ? nullptr : out_images_.data();
            out->image_count = (int)out_images_.size();
            // xi_record_out::json is char* (plugin-owned, non-const ABI).
            // out_json_ outlives this call (member of the adapter), so
            // exposing data() is safe; nothing writes to it.
            out->json        = out_json_.data();
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

    // Reader-thread-driven send + wait. The reader owns the pipe's
    // read side; do_call_locked_ only writes (under pipe_write_mu_)
    // and waits on a per-seq promise. This lets seq=0 async frames
    // (RPC_EMIT_TRIGGER) reach the bus the instant they arrive, even
    // when no backend→worker RPC is in flight.
    ipc::Frame do_call_locked_(uint32_t type, const std::vector<uint8_t>& payload) {
        const int wait_ms = call_timeout_ms_;

        // Reserve sequence + promise BEFORE writing. If the reply
        // somehow lands between write and registration the reader
        // would discard it as orphan; registering first closes that
        // race.
        uint32_t seq = ++seq_;
        auto entry = std::make_shared<InflightEntry>();
        std::future<ipc::Frame> fut = entry->prom.get_future();
        {
            std::lock_guard<std::mutex> lk(inflight_mu_);
            if (reader_dead_) {
                throw std::runtime_error("worker pipe dead: " + reader_err_);
            }
            inflight_[seq] = entry;
        }

        // Send. Holding pipe_write_mu_ matters once we lift mu_ for
        // burst parallelism (task #71); today mu_ already serialises
        // callers but the dedicated write mutex documents intent and
        // is cheap.
        try {
            std::lock_guard<std::mutex> wlk(pipe_write_mu_);
            ipc::send_frame(pipe_, seq, type,
                            payload.data(), (uint32_t)payload.size());
        } catch (...) {
            // Send failed — the reader will likely also see a dead
            // pipe shortly. Clean our entry up so we don't leak.
            std::lock_guard<std::mutex> lk(inflight_mu_);
            inflight_.erase(seq);
            throw;
        }

        // Wait for the reader to fulfil the promise (or for timeout).
        if (fut.wait_for(std::chrono::milliseconds(wait_ms))
                != std::future_status::ready) {
            // Pop the entry first — if the reader is mid-fulfil (the
            // reply just arrived right at the deadline) it will see
            // the entry already gone and log+drop. Either way our
            // future will not be set after this point.
            {
                std::lock_guard<std::mutex> lk(inflight_mu_);
                auto it = inflight_.find(seq);
                if (it == inflight_.end()) {
                    // Reader already popped + fulfilled. Race won by
                    // the reader; honour the real reply.
                    return fut.get();
                }
                inflight_.erase(it);
            }
            // Hung worker. Cancel the reader's blocking ReadFile so
            // it exits + the call_() catch can respawn.
            //
            // CancelIoEx(h, nullptr) cancels ALL pending I/O on the
            // handle. On a non-overlapped pipe this is the only way
            // to unblock the reader's ReadFile; the worker is wedged
            // anyway so nuking the connection is the right move.
            HANDLE ph = pipe_.native_handle();
            if (ph != INVALID_HANDLE_VALUE) CancelIoEx(ph, nullptr);
            throw std::runtime_error(
                "worker call timeout (" + std::to_string(wait_ms) + "ms)");
        }

        // Either a reply or a reader-side exception (broken pipe etc.)
        // get() rethrows whatever the reader stored.
        return fut.get();
    }

    // Backwards-compat alias used by try_respawn_locked_'s set_def
    // restoration path. Same semantics as do_call_locked_.
    ipc::Frame raw_call_locked_(uint32_t type, const std::vector<uint8_t>& payload) {
        return do_call_locked_(type, payload);
    }

    // ---- Always-on reader thread --------------------------------------
    //
    // Why a dedicated reader, after the prior attempt was rolled back:
    //
    // The earlier reader-thread implementation (rolled back before
    // PR #19) "spuriously" reported ERROR_BROKEN_PIPE on the second
    // recv. The actual cause was that `do_call_locked_` was ALSO
    // calling `recv_frame` directly. Two threads doing blocking
    // ReadFile on a byte-mode named pipe split the byte stream
    // arbitrarily — one thread would consume part of frame N's
    // header, the other the rest, magic mismatched, the wrapper
    // throws, the pipe gets closed mid-stream, and the next read
    // sees ERROR_BROKEN_PIPE. This design fixes that by making the
    // reader the SOLE reader; do_call_locked_ never calls
    // recv_frame, it waits on a promise.
    //
    // Pipe is non-overlapped (`PIPE_WAIT`, no FILE_FLAG_OVERLAPPED —
    // see xi_ipc.hpp Pipe::accept_one). Concurrent read+write on the
    // same handle is supported by Win32 named pipes; concurrent
    // readers are not. We rely on this.
    //
    // Shutdown: `stop_reader_()` flips `stopping_` then `CancelIoEx`s
    // the pipe to unblock the reader's ReadFile. The reader sees a
    // throw, observes `stopping_`, exits cleanly, and we join.
    //
    // Worker-died vs we-asked-to-stop: distinguished by `stopping_`.
    // If `stopping_` is true when the reader's ReadFile throws, it's
    // shutdown — exit silently. Otherwise it's "worker died" —
    // record the error, fail every in-flight promise, set
    // `reader_dead_` so future do_call_locked_ throws fast (lets
    // call_() proceed to try_respawn_locked_).
    void start_reader_() {
        stopping_       = false;
        reader_dead_    = false;
        reader_err_.clear();
        reader_ = std::thread([this] { run_reader_(); });
    }

    void stop_reader_() {
        stopping_ = true;
        // Wake the reader's blocking ReadFile. CancelIoEx returns
        // ERROR_NOT_FOUND if no I/O is pending — that's fine.
        // TODO(linux): epoll-based reader; eventfd to wake. Pipe is
        // Win32-only today (see docs/design/linux-port.md).
#ifdef _WIN32
        HANDLE ph = pipe_.native_handle();
        if (ph != INVALID_HANDLE_VALUE) CancelIoEx(ph, nullptr);
#endif
        if (reader_.joinable()) reader_.join();
        // Fail every still-pending promise so callers don't hang.
        std::lock_guard<std::mutex> lk(inflight_mu_);
        for (auto& [seq, e] : inflight_) {
            try {
                e->prom.set_exception(std::make_exception_ptr(
                    std::runtime_error("worker pipe shut down")));
            } catch (const std::future_error&) {}
        }
        inflight_.clear();
    }

    void run_reader_() {
        for (;;) {
            ipc::Frame f;
            try {
                f = ipc::recv_frame(pipe_);
            } catch (const std::exception& e) {
                if (stopping_) return;          // clean shutdown
                // Worker died (or hung + watchdog cancelled us).
                std::lock_guard<std::mutex> lk(inflight_mu_);
                reader_dead_ = true;
                reader_err_  = e.what();
                for (auto& [seq, ent] : inflight_) {
                    try {
                        ent->prom.set_exception(std::make_exception_ptr(
                            std::runtime_error(std::string("worker pipe: ")
                                               + e.what())));
                    } catch (const std::future_error&) {}
                }
                inflight_.clear();
                return;
            }
            if (f.seq == 0) {
                // One-way async frame from the worker — dispatch
                // straight to the trigger bus. No promise to fulfil,
                // no reply to send. handle_async_frame_ must be
                // non-blocking; TriggerBus::emit pushes onto the
                // dispatch queue and returns.
                try { handle_async_frame_(f); }
                catch (const std::exception& e) {
                    std::fprintf(stderr,
                        "[ProcessInstanceAdapter] '%s' async dispatch threw: %s\n",
                        name_.c_str(), e.what());
                }
                continue;
            }
            // Reply for an in-flight call. Pop and fulfil.
            std::shared_ptr<InflightEntry> ent;
            {
                std::lock_guard<std::mutex> lk(inflight_mu_);
                auto it = inflight_.find(f.seq);
                if (it != inflight_.end()) {
                    ent = it->second;
                    inflight_.erase(it);
                }
            }
            if (ent) {
                try { ent->prom.set_value(std::move(f)); }
                catch (const std::future_error&) {}
            } else {
                std::fprintf(stderr,
                    "[ProcessInstanceAdapter] '%s' orphan reply seq=%u — dropped\n",
                    name_.c_str(), (unsigned)f.seq);
            }
        }
    }

    // Handle a worker-initiated one-way frame (seq=0). Currently the
    // only one is RPC_EMIT_TRIGGER from source plugins — forward into
    // the backend's TriggerBus singleton. Image handles in the payload
    // are SHM-tagged (worker promoted them before sending) so the bus
    // can deref them through host_api.
    void handle_async_frame_(const ipc::Frame& f) {
        switch (f.type) {
        case ipc::RPC_EMIT_TRIGGER: {
            ipc::Reader r(f.payload);
            auto src_bytes = r.bytes();
            std::string source(src_bytes.begin(), src_bytes.end());
            uint64_t tid_hi = r.u64();
            uint64_t tid_lo = r.u64();
            int64_t  ts_us  = (int64_t)r.u64();
            uint32_t n_img  = r.u32();
            std::vector<std::string>     keys;
            std::vector<xi_record_image> imgs;
            keys.reserve(n_img);
            imgs.reserve(n_img);
            for (uint32_t i = 0; i < n_img; ++i) {
                auto k_bytes = r.bytes();
                uint64_t h   = r.u64();
                keys.emplace_back(k_bytes.begin(), k_bytes.end());
                xi_record_image rec{};
                rec.key    = keys.back().c_str();
                rec.handle = h;
                imgs.push_back(rec);
            }
            xi_trigger_id tid{tid_hi, tid_lo};
            TriggerBus::instance().emit(source, tid, ts_us,
                                         imgs.empty() ? nullptr : imgs.data(),
                                         (int32_t)imgs.size());
            break;
        }
        default:
            std::fprintf(stderr,
                "[ProcessInstanceAdapter] '%s' got async frame type=%u — ignored\n",
                name_.c_str(), (unsigned)f.type);
            break;
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

        // Tear down reader before we drop the pipe — otherwise the
        // reader holds onto a HANDLE we're about to invalidate.
        stop_reader_();
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
        if (!instance_folder_.empty()) {
            cmd += " --instance-folder=\"" + instance_folder_ + "\"";
        }

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
        start_reader_();

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
        stop_reader_();
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
    // `mu_` serialises calls — only one in-flight RPC per adapter.
    // Burst parallelism (task #71) will lift this to a worker pool;
    // for now keep it simple.
    mutable std::mutex mu_;
    uint32_t    seq_ = 0;
    std::atomic<bool> dead_{false};

    // ---- Reader thread machinery (see start_reader_ / run_reader_) ---
    // Per in-flight call: a promise the reader fulfils when the
    // matching reply arrives (or set_exception's on broken pipe /
    // shutdown). Held by shared_ptr so do_call_locked_ keeps its
    // future alive even if the reader pops the entry concurrently.
    struct InflightEntry {
        std::promise<ipc::Frame> prom;
    };
    std::thread        reader_;
    std::atomic<bool>  stopping_{false};
    bool               reader_dead_ = false;     // guarded by inflight_mu_
    std::string        reader_err_;              // ditto
    std::mutex         inflight_mu_;
    std::unordered_map<uint32_t, std::shared_ptr<InflightEntry>> inflight_;
    // Two writers can hit the host-side pipe send path — currently
    // there is only one (do_call_locked_ holds mu_) but burst
    // parallelism (task #71) will lift mu_. The dedicated write
    // mutex documents the invariant up front.
    std::mutex         pipe_write_mu_;

    // Spawn parameters — cached so try_respawn_locked_ can re-spawn
    // identical workers without the caller having to pass them again.
    std::filesystem::path worker_exe_path_;
    std::filesystem::path plugin_dll_path_;
    std::string           shm_name_;
    std::string           instance_folder_;

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
    // Per-call output storage. The xi_record_image array's `key` field
    // is `const char*` borrowing into out_keys_ — vector reallocation
    // would invalidate the pointers, so we reserve before populating.
    std::vector<xi_record_image> out_images_;
    std::vector<std::string>     out_keys_;
    std::string                  out_json_;
};

} // namespace xi
