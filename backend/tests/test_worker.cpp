//
// test_worker.cpp — end-to-end Phase-2 validation.
//
// Stands in for the backend: creates a SHM region, allocates an input
// image, spawns xinsp-worker.exe pointing at the test plugin DLL, sends
// CREATE → PROCESS → DESTROY, and verifies the output bytes match the
// expected doubled pattern.
//
// Proves the spike's central claim: pixel data crosses the backend ↔
// worker process boundary WITHOUT any memcpy. Both processes mmap the
// same SHM pages; only handle integers + tiny JSON ride the pipe.
//

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <xi/xi_ipc.hpp>
#include <xi/xi_shm.hpp>

namespace ipc = xi::ipc;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static uint8_t pat(int x, int y) { return (uint8_t)((x * 3 + y * 5) & 0x7F); }

// Spawn xinsp-worker.exe pointing at our pipe + shm + plugin dll.
// Returns the process handle (caller should WaitForSingleObject + close).
static HANDLE spawn_worker(const std::string& worker_exe,
                           const std::string& pipe_name,
                           const std::string& shm_name,
                           const std::string& plugin_dll) {
    std::string cmd = "\"" + worker_exe + "\"" +
        " --pipe=" + pipe_name +
        " --shm=" + shm_name +
        " --plugin-dll=\"" + plugin_dll + "\"" +
        " --instance=test0";

    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "CreateProcess(worker) failed: %lu\n", GetLastError());
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

int main(int argc, char** argv) {
    // Locate worker.exe + plugin dll — same Release dir we live in.
    char self[MAX_PATH]; GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::filesystem::path here = std::filesystem::path(self).parent_path();
    std::string worker_exe = (here / "xinsp-worker.exe").string();
    std::string plugin_dll = (here / "test_worker_plugin.dll").string();

    if (!std::filesystem::exists(worker_exe)) {
        std::fprintf(stderr, "missing %s\n", worker_exe.c_str()); return 2;
    }
    if (!std::filesystem::exists(plugin_dll)) {
        std::fprintf(stderr, "missing %s\n", plugin_dll.c_str()); return 2;
    }

    // 1. Create the SHM region and the input image.
    char shm_name[64];  std::snprintf(shm_name,  sizeof(shm_name),
                                       "xinsp2-shm-tw-%lu",  GetCurrentProcessId());
    char pipe_name[64]; std::snprintf(pipe_name, sizeof(pipe_name),
                                       "xinsp2-tw-%lu",      GetCurrentProcessId());

    auto shm = xi::ShmRegion::create(shm_name, 16 * 1024 * 1024);
    const int W = 64, H = 32;
    uint64_t in_h = shm.alloc_image(W, H, 1);
    CHECK(in_h != 0);
    uint8_t* in_px = shm.data(in_h);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            in_px[y * W + x] = pat(x, y);

    std::fprintf(stderr, "[test] shm=%s pipe=%s in_h=0x%llx\n",
                 shm_name, pipe_name, (unsigned long long)in_h);

    // 2. Pipe must be ACCEPT-ed before the worker tries to connect.
    //    We start the accept on a separate thread, then spawn the worker.
    HANDLE worker_proc = spawn_worker(worker_exe, pipe_name, shm_name, plugin_dll);
    CHECK(worker_proc != nullptr);
    if (!worker_proc) return 3;

    ipc::Pipe pipe;
    try { pipe = ipc::Pipe::accept_one(pipe_name); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "[test] accept failed: %s\n", e.what());
        return 4;
    }
    std::fprintf(stderr, "[test] pipe accepted\n");

    auto rpc_call = [&](uint32_t type, const std::vector<uint8_t>& payload) -> ipc::Frame {
        static uint32_t s_seq = 100;
        uint32_t seq = ++s_seq;
        ipc::send_frame(pipe, seq, type, payload.data(), (uint32_t)payload.size());
        ipc::Frame r = ipc::recv_frame(pipe);
        CHECK(r.seq == seq);
        if (r.type == ipc::RPC_TYPE_ERROR) {
            std::string msg(r.payload.begin(), r.payload.end());
            std::fprintf(stderr, "[test] RPC %u error: %s\n", type, msg.c_str());
        }
        return r;
    };

    // CREATE
    {
        ipc::Writer w; w.str("test0"); w.str(plugin_dll);
        auto r = rpc_call(ipc::RPC_CREATE, w.buf());
        CHECK(r.type == (ipc::RPC_CREATE | ipc::RPC_REPLY_BIT));
    }

    // GET_DEF
    {
        auto r = rpc_call(ipc::RPC_GET_DEF, {});
        CHECK(r.type == (ipc::RPC_GET_DEF | ipc::RPC_REPLY_BIT));
        ipc::Reader rd(r.payload);
        auto def = rd.bytes();
        std::string s(def.begin(), def.end());
        CHECK(s == "{}");
    }

    // EXCHANGE
    {
        ipc::Writer w; w.str("hello");
        auto r = rpc_call(ipc::RPC_EXCHANGE, w.buf());
        CHECK(r.type == (ipc::RPC_EXCHANGE | ipc::RPC_REPLY_BIT));
        ipc::Reader rd(r.payload);
        auto rsp = rd.bytes();
        std::string s(rsp.begin(), rsp.end());
        CHECK(s.find("doubler") != std::string::npos);
    }

    // PROCESS — the meat. Hand the worker our input handle; receive an
    // output handle that points to bytes the worker wrote into shared
    // memory in its own process. NO memcpy happens here.
    uint64_t out_h = 0;
    {
        ipc::Writer w; w.u64(in_h); w.bytes("{}", 2);
        auto r = rpc_call(ipc::RPC_PROCESS, w.buf());
        CHECK(r.type == (ipc::RPC_PROCESS | ipc::RPC_REPLY_BIT));
        ipc::Reader rd(r.payload);
        out_h = rd.u64();
        rd.bytes(); // out json (unused)
    }
    CHECK(out_h != 0);
    CHECK(shm.is_valid_handle(out_h));
    CHECK(shm.width(out_h) == W);
    CHECK(shm.height(out_h) == H);
    CHECK(shm.channels(out_h) == 1);

    // Verify the doubled pattern.
    const uint8_t* out_px = shm.data(out_h);
    int wrong = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t in_v = pat(x, y);
            uint8_t exp  = (uint8_t)(in_v * 2 > 255 ? 255 : in_v * 2);
            if (out_px[y * W + x] != exp) ++wrong;
        }
    }
    CHECK(wrong == 0);
    std::fprintf(stderr, "[test] %s — %d wrong pixels of %d\n",
                 wrong == 0 ? "ZERO-COPY ROUND TRIP OK" : "MISMATCH",
                 wrong, W * H);

    // Cross-process refcount: backend (us) addrefs the worker-allocated
    // handle, then we release twice — should drop to 0.
    shm.addref(out_h);
    CHECK(shm.refcount(out_h) >= 2);
    shm.release(out_h);
    shm.release(out_h);

    // DESTROY tells the worker to clean up + exit.
    {
        auto r = rpc_call(ipc::RPC_DESTROY, {});
        CHECK(r.type == (ipc::RPC_DESTROY | ipc::RPC_REPLY_BIT));
    }

    // Wait for the worker process to exit.
    DWORD wait = WaitForSingleObject(worker_proc, 5000);
    CHECK(wait == WAIT_OBJECT_0);
    DWORD code = 0; GetExitCodeProcess(worker_proc, &code);
    CHECK(code == 0);
    CloseHandle(worker_proc);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_worker: ALL PASS\n");
        return 0;
    } else {
        std::fprintf(stderr, "test_worker: %d FAIL\n", g_failures);
        return 1;
    }
}
