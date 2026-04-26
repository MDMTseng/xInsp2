//
// test_script_runner.cpp — Phase 3 minimum E2E.
//
// Spawns xinsp-script-runner.exe pointing at a tiny inspection-script
// DLL, sends RPC_SCRIPT_RUN(frame=7), parses the snapshot JSON, and
// verifies the script ran in the runner's address space.
//

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include <xi/xi_ipc.hpp>
#include <xi/xi_shm.hpp>

namespace ipc = xi::ipc;
namespace fs  = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

int main() {
    char self[MAX_PATH]; GetModuleFileNameA(nullptr, self, MAX_PATH);
    fs::path here       = fs::path(self).parent_path();
    fs::path runner_exe = here / "xinsp-script-runner.exe";
    fs::path script_dll = here / "test_script_runner_dll.dll";
    if (!fs::exists(runner_exe) || !fs::exists(script_dll)) {
        std::fprintf(stderr, "missing prereqs: runner=%d dll=%d\n",
                     (int)fs::exists(runner_exe), (int)fs::exists(script_dll));
        return 2;
    }

    char shm_name[64];
    char pipe_name[64];
    std::snprintf(shm_name,  sizeof(shm_name),  "xinsp2-shm-tsr-%lu",
                  (unsigned long)GetCurrentProcessId());
    std::snprintf(pipe_name, sizeof(pipe_name), "xinsp2-tsr-%lu",
                  (unsigned long)GetCurrentProcessId());

    auto shm = xi::ShmRegion::create(shm_name, 8 * 1024 * 1024);
    (void)shm;  // not used directly here, but the runner attaches it

    // Spawn runner.
    std::string cmd = "\"" + runner_exe.string() + "\""
        + " --pipe="  + pipe_name
        + " --shm="   + shm_name
        + " --script-dll=\"" + script_dll.string() + "\"";
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        return 3;
    }
    HANDLE proc = pi.hProcess; CloseHandle(pi.hThread);

    ipc::Pipe pipe;
    try { pipe = ipc::Pipe::accept_one(pipe_name); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "accept failed: %s\n", e.what());
        TerminateProcess(proc, 1); CloseHandle(proc);
        return 4;
    }
    std::fprintf(stderr, "[test] pipe accepted\n");

    auto rpc = [&](uint32_t type, const std::vector<uint8_t>& payload) {
        static uint32_t s_seq = 100;
        uint32_t seq = ++s_seq;
        ipc::send_frame(pipe, seq, type, payload.data(), (uint32_t)payload.size());
        return ipc::recv_frame(pipe);
    };

    // RUN with frame = 7.
    {
        ipc::Writer w; w.u32(7);
        auto r = rpc(ipc::RPC_SCRIPT_RUN, w.buf());
        CHECK(r.type == (ipc::RPC_SCRIPT_RUN | ipc::RPC_REPLY_BIT));
        ipc::Reader rd(r.payload);
        auto vars = rd.bytes();
        std::string vars_json(vars.begin(), vars.end());
        std::fprintf(stderr, "[test] vars after frame=7: %s\n", vars_json.c_str());
        CHECK(vars_json.find("\"frame_doubled\"") != std::string::npos);
        CHECK(vars_json.find("\"value\":14") != std::string::npos);
    }

    // Second run: frame = 21 → doubled 42. Validates that reset cleared
    // state from the previous call (we'd see stale 14 if reset broke).
    {
        ipc::Writer w; w.u32(21);
        auto r = rpc(ipc::RPC_SCRIPT_RUN, w.buf());
        CHECK(r.type == (ipc::RPC_SCRIPT_RUN | ipc::RPC_REPLY_BIT));
        ipc::Reader rd(r.payload);
        auto vars = rd.bytes();
        std::string vars_json(vars.begin(), vars.end());
        std::fprintf(stderr, "[test] vars after frame=21: %s\n", vars_json.c_str());
        CHECK(vars_json.find("\"value\":42") != std::string::npos);
    }

    // DESTROY → runner exits 0.
    {
        auto r = rpc(ipc::RPC_DESTROY, {});
        CHECK(r.type == (ipc::RPC_DESTROY | ipc::RPC_REPLY_BIT));
    }
    DWORD wait = WaitForSingleObject(proc, 5000);
    CHECK(wait == WAIT_OBJECT_0);
    DWORD code = 0; GetExitCodeProcess(proc, &code);
    CHECK(code == 0);
    CloseHandle(proc);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_script_runner: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_script_runner: %d FAIL\n", g_failures);
    return 1;
}
