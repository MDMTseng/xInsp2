#pragma once
//
// xi_ipc.hpp — minimal request/reply IPC over Windows named pipes.
//
// Used to drive a `xinsp-worker.exe` process that hosts ONE plugin
// instance. Backend (client) sends a request, worker (server) replies.
// All bulk data (images) goes through xi_shm — frames here only carry
// handles + small JSON.
//
// Frame format (little-endian, all uint32 unless noted):
//
//   [magic][seq][type][len][payload of len bytes]
//
//     magic = 0x49504958  ('XIPI')
//     seq   = request id; reply MUST echo it
//     type  = RpcType (request) or RpcType | RPC_REPLY_BIT (reply)
//             0xFF = error reply (payload = utf-8 message)
//     len   = payload byte count
//
// One thread per pipe. The backend serialises calls per worker.
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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace xi::ipc {

constexpr uint32_t FRAME_MAGIC    = 0x49504958u;   // 'XIPI'
constexpr uint32_t RPC_REPLY_BIT  = 0x80000000u;
constexpr uint32_t RPC_TYPE_ERROR = 0xFFu;

// One-instance-per-worker RPC menu. Worker creates the plugin on its
// first CREATE request and serves the rest until DESTROY (or EOF).
enum RpcType : uint32_t {
    RPC_CREATE   = 1,   // payload: u32 name_len + name + u32 dll_len + dll_path
    RPC_DESTROY  = 2,   // payload: empty
    RPC_PROCESS  = 3,   // payload: u64 in_image_handle + u32 json_len + json
                        // reply  : u64 out_image_handle + u32 json_len + json
    RPC_EXCHANGE = 4,   // payload: u32 cmd_len + cmd
                        // reply  : u32 rsp_len + rsp
    RPC_GET_DEF  = 5,   // payload: empty
                        // reply  : u32 def_len + def
    RPC_SET_DEF  = 6,   // payload: u32 def_len + def
                        // reply  : u8 ok

    // --- script-runner RPCs (Phase 3 minimal slice) -------------------
    // The runner hosts an inspection script DLL; backend drives it over
    // the same pipe. use_*/trigger/breakpoint/params/state proxying is
    // deferred — Phase 3 minimum is: ask the runner to RESET + run
    // xi_inspect_entry once, then snapshot ValueStore back as JSON.
    RPC_SCRIPT_RUN  = 7,   // payload: u32 frame_num
                           // reply  : u32 vars_len + vars_json

    // --- Phase 3.5: script→backend callback proxy ---------------------
    // When a script in the runner calls xi::use<T>("name").process(...),
    // the runner sends this back to the backend over the SAME pipe; the
    // backend dispatches to its local instance (or the instance's own
    // ProcessInstanceAdapter for triple-hop) and replies with a SHM
    // output handle. This is a runner→backend request, not the usual
    // backend→runner.
    RPC_USE_PROCESS  = 8,  // payload: u32 name_len + name
                           //          u64 input_handle + u32 json_len + json
                           // reply  : u64 output_handle + u32 json_len + json

    // Test-only: drive a scratch input handle into the running script.
    // Lets the test driver hand the script an SHM image to feed into
    // its first xi::use<>().process() call.
    RPC_TEST_SET_INPUT = 9, // payload: u64 input_handle
                            // reply  : u8 ok
};

struct FrameHeader {
    uint32_t magic;
    uint32_t seq;
    uint32_t type;
    uint32_t len;
};
static_assert(sizeof(FrameHeader) == 16, "FrameHeader must be 16 bytes");

// ----- Pipe RAII wrapper ------------------------------------------------
//
// Both ends use the same Pipe object after connection. Server creates
// the pipe + waits for connect; client opens it. Once connected each
// side reads/writes frames synchronously.

class Pipe {
public:
    Pipe() = default;
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe(Pipe&& o) noexcept { *this = std::move(o); }
    Pipe& operator=(Pipe&& o) noexcept {
        if (this != &o) { close_(); h_ = o.h_; o.h_ = INVALID_HANDLE_VALUE; }
        return *this;
    }
    ~Pipe() { close_(); }

    // Server: create the pipe and block until a client connects.
    static Pipe accept_one(const std::string& name) {
#ifdef _WIN32
        std::string full = R"(\\.\pipe\)" + name;
        HANDLE h = CreateNamedPipeA(
            full.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,                  // maxInstances
            64 * 1024,          // outBufferSize
            64 * 1024,          // inBufferSize
            0,                  // defaultTimeout
            nullptr);
        if (h == INVALID_HANDLE_VALUE)
            throw std::runtime_error("CreateNamedPipeA failed");

        BOOL connected = ConnectNamedPipe(h, nullptr) ?
            TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(h);
            throw std::runtime_error("ConnectNamedPipe failed");
        }
        Pipe p; p.h_ = h; return p;
#else
        (void)name; throw std::runtime_error("xi_ipc Windows-only in this spike");
#endif
    }

    // Client: connect to an existing pipe. Retries briefly while the
    // server is still initialising.
    static Pipe connect(const std::string& name, int timeout_ms = 5000) {
#ifdef _WIN32
        std::string full = R"(\\.\pipe\)" + name;
        DWORD start = GetTickCount();
        for (;;) {
            HANDLE h = CreateFileA(full.c_str(),
                GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                Pipe p; p.h_ = h; return p;
            }
            DWORD err = GetLastError();
            if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND)
                throw std::runtime_error("CreateFileA pipe failed: " + std::to_string(err));
            if ((int)(GetTickCount() - start) > timeout_ms)
                throw std::runtime_error("pipe connect timed out: " + name);
            Sleep(20);
        }
#else
        (void)name; (void)timeout_ms;
        throw std::runtime_error("xi_ipc Windows-only in this spike");
#endif
    }

    // Block until exactly `n` bytes are read. Throws on EOF / pipe error.
    void read_exact(void* buf, size_t n) {
        uint8_t* p = (uint8_t*)buf;
        while (n) {
            DWORD got = 0;
            if (!ReadFile(h_, p, (DWORD)n, &got, nullptr) || got == 0)
                throw std::runtime_error("pipe read EOF");
            p += got; n -= got;
        }
    }

    void write_all(const void* buf, size_t n) {
        const uint8_t* p = (const uint8_t*)buf;
        while (n) {
            DWORD wrote = 0;
            if (!WriteFile(h_, p, (DWORD)n, &wrote, nullptr) || wrote == 0)
                throw std::runtime_error("pipe write failed");
            p += wrote; n -= wrote;
        }
    }

    bool valid() const { return h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    void close_() {
        if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
    }
};

// ----- Frame send / recv ------------------------------------------------

inline void send_frame(Pipe& p, uint32_t seq, uint32_t type,
                       const void* payload, uint32_t len) {
    FrameHeader h{ FRAME_MAGIC, seq, type, len };
    p.write_all(&h, sizeof(h));
    if (len) p.write_all(payload, len);
}

// Reply convenience: send the request's seq with type | RPC_REPLY_BIT.
inline void send_reply(Pipe& p, uint32_t req_seq, uint32_t req_type,
                       const void* payload, uint32_t len) {
    send_frame(p, req_seq, req_type | RPC_REPLY_BIT, payload, len);
}

inline void send_error_reply(Pipe& p, uint32_t req_seq, const std::string& msg) {
    send_frame(p, req_seq, RPC_TYPE_ERROR, msg.data(), (uint32_t)msg.size());
}

struct Frame {
    uint32_t seq;
    uint32_t type;
    std::vector<uint8_t> payload;
};

inline Frame recv_frame(Pipe& p) {
    FrameHeader h;
    p.read_exact(&h, sizeof(h));
    if (h.magic != FRAME_MAGIC)
        throw std::runtime_error("frame magic mismatch");
    Frame f;
    f.seq = h.seq;
    f.type = h.type;
    f.payload.resize(h.len);
    if (h.len) p.read_exact(f.payload.data(), h.len);
    return f;
}

// ----- Session: bidirectional RPC over a single pipe ------------------
//
// Both ends use a Session. Calling code does:
//
//   Frame reply = sess.call(type, payload);
//
// Internally call() sends, then reads frames in a loop until it gets
// the matching reply seq. If a frame arrives that's a REQUEST from the
// other end (no REPLY_BIT), it's dispatched through the registered
// handler and the result is sent back as a reply — then the loop keeps
// looking for the original reply. This lets a script running in the
// runner call xi::use(...) which in turn RPCs back into the backend
// while the backend is still waiting for the script's main reply.
//
// Single-threaded per session: pipe I/O is serialised by mu_. The
// handler runs synchronously inside loop_until_reply_; it must NOT
// re-enter call() on the same Session.

class Session {
public:
    // Returns reply payload bytes. Throw to send an error reply.
    using Handler = std::function<std::vector<uint8_t>(uint32_t type,
                                                       const std::vector<uint8_t>& payload)>;

    Session() = default;
    explicit Session(Pipe&& p) : pipe_(std::move(p)) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Non-movable: contains std::mutex. Heap-allocate via unique_ptr if
    // you need late binding.
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    bool valid() const { return pipe_.valid(); }
    void set_handler(Handler h) { handler_ = std::move(h); }

    // Direct access to the underlying pipe — useful when you want to
    // run a custom dispatch loop alongside Session-based call()s. Callers
    // that mix raw pipe reads with sess.call() must serialise the I/O
    // themselves; the Session's mutex doesn't cover external reads.
    Pipe& pipe() { return pipe_; }

    // Blocking call. Returns the reply frame (type has REPLY_BIT set,
    // or RPC_TYPE_ERROR for a remote error).
    Frame call(uint32_t type, const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lk(mu_);
        uint32_t seq = ++seq_;
        send_frame(pipe_, seq, type, payload.data(), (uint32_t)payload.size());
        return loop_until_reply_(seq);
    }

    // Server-side: keep handling incoming requests forever (or until
    // the pipe closes). For sessions that only RECEIVE requests; if
    // the side may also call(), use call() / loop_until_reply_ instead.
    void serve_forever() {
        while (true) {
            Frame f = recv_frame(pipe_);
            if ((f.type & RPC_REPLY_BIT) || f.type == RPC_TYPE_ERROR) {
                // Orphan reply on a serve-only session.
                continue;
            }
            dispatch_(f);
        }
    }

private:
    Frame loop_until_reply_(uint32_t want_seq) {
        for (;;) {
            Frame f = recv_frame(pipe_);
            bool is_reply = (f.type & RPC_REPLY_BIT) || f.type == RPC_TYPE_ERROR;
            if (is_reply) {
                if (f.seq != want_seq)
                    throw std::runtime_error("orphan reply seq " + std::to_string(f.seq));
                return f;
            }
            // Incoming nested request — dispatch + reply, then keep waiting.
            dispatch_(f);
        }
    }

    void dispatch_(const Frame& f) {
        std::vector<uint8_t> reply;
        std::string err;
        try {
            if (handler_) reply = handler_(f.type, f.payload);
            else          err = "no handler installed";
        } catch (const std::exception& e) { err = e.what(); }
        if (!err.empty()) send_error_reply(pipe_, f.seq, err);
        else              send_reply(pipe_, f.seq, f.type,
                                     reply.data(), (uint32_t)reply.size());
    }

    Pipe       pipe_;
    std::mutex mu_;
    uint32_t   seq_ = 0;
    Handler    handler_;
};

// ----- Tiny binary cursor for payload encode/decode --------------------
//
// Plugin RPCs stay binary because we want a) zero parse overhead on the
// hot path and b) to pass image handles as raw uint64 not stringified.

class Writer {
public:
    void u32(uint32_t v) { append(&v, 4); }
    void u64(uint64_t v) { append(&v, 8); }
    void u8 (uint8_t  v) { append(&v, 1); }
    void str(const std::string& s) {
        u32((uint32_t)s.size()); if (!s.empty()) append(s.data(), s.size());
    }
    void bytes(const void* p, size_t n) {
        u32((uint32_t)n); if (n) append(p, n);
    }
    const std::vector<uint8_t>& buf() const { return b_; }
private:
    void append(const void* p, size_t n) {
        size_t old = b_.size();
        b_.resize(old + n);
        std::memcpy(b_.data() + old, p, n);
    }
    std::vector<uint8_t> b_;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    Reader(const std::vector<uint8_t>& v) : Reader(v.data(), v.size()) {}

    uint32_t u32() { uint32_t v; take(&v, 4); return v; }
    uint64_t u64() { uint64_t v; take(&v, 8); return v; }
    uint8_t  u8 () { uint8_t  v; take(&v, 1); return v; }
    std::string str() {
        uint32_t n = u32();
        std::string s; s.assign((const char*)p_, n);
        p_ += n; return s;
    }
    std::vector<uint8_t> bytes() {
        uint32_t n = u32();
        std::vector<uint8_t> v(p_, p_ + n);
        p_ += n; return v;
    }
private:
    void take(void* dst, size_t n) {
        if ((size_t)(end_ - p_) < n) throw std::runtime_error("ipc: short payload");
        std::memcpy(dst, p_, n); p_ += n;
    }
    const uint8_t* p_;
    const uint8_t* end_;
};

} // namespace xi::ipc
