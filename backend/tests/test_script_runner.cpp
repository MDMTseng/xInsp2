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

#include <xi/xi_image_pool.hpp>
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
    xi::ImagePool::set_shm_region(&shm);

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

    ipc::Session sess(ipc::Pipe::accept_one(pipe_name));
    std::fprintf(stderr, "[test] pipe accepted\n");

    // The runner forwards script-side use_process calls back to us via
    // RPC_USE_PROCESS. We act as the "backend" — for the test this is
    // a hard-coded doubler that mirrors test_worker_plugin's logic.
    sess.set_handler([&](uint32_t type, const std::vector<uint8_t>& payload)
                     -> std::vector<uint8_t> {
        if (type == ipc::RPC_USE_PROCESS) {
            ipc::Reader r(payload);
            std::string name = r.str();
            uint64_t in_h = r.u64();
            auto in_json = r.bytes();
            std::fprintf(stderr,
                "[test/handler] use_process(name=%s, in_h=0x%llx, json=%.*s)\n",
                name.c_str(), (unsigned long long)in_h,
                (int)in_json.size(), in_json.data());

            // Allocate output in SHM (zero-copy back to the script).
            int w = shm.width(in_h), h = shm.height(in_h), ch = shm.channels(in_h);
            uint64_t out_h = shm.alloc_image(w, h, ch);
            const uint8_t* sp = shm.data(in_h);
            uint8_t*       dp = shm.data(out_h);
            const int total = w * h * ch;
            for (int i = 0; i < total; ++i) {
                int v = sp[i] * 2;
                dp[i] = (uint8_t)(v > 255 ? 255 : v);
            }
            ipc::Writer w_out;
            w_out.u64(out_h);
            w_out.bytes("{\"who\":\"test/handler\"}", 21);
            return w_out.buf();
        }
        if (type == ipc::RPC_USE_EXCHANGE) {
            ipc::Reader r(payload);
            std::string name = r.str();
            std::string cmd  = r.str();
            std::fprintf(stderr,
                "[test/handler] use_exchange(name=%s, cmd=%s)\n",
                name.c_str(), cmd.c_str());
            ipc::Writer w_out;
            std::string rsp = "{\"who\":\"doubler\",\"got\":" + std::to_string(cmd.size()) + "}";
            w_out.bytes(rsp.data(), rsp.size());
            return w_out.buf();
        }
        if (type == ipc::RPC_USE_GRAB) {
            ipc::Reader r(payload);
            std::string name = r.str();
            uint32_t timeout_ms = r.u32();
            std::fprintf(stderr,
                "[test/handler] use_grab(name=%s, timeout=%u)\n",
                name.c_str(), timeout_ms);
            // Allocate a fresh SHM image, fill with sentinel 0x42 so
            // the script side can verify "yes that came from the host".
            uint64_t h = shm.alloc_image(8, 8, 1);
            std::memset(shm.data(h), 0x42, 64);
            ipc::Writer w_out; w_out.u64(h);
            return w_out.buf();
        }
        throw std::runtime_error("unexpected nested rpc " + std::to_string(type));
    });

    auto rpc = [&](uint32_t type, const std::vector<uint8_t>& payload) {
        return sess.call(type, payload);
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

    // ---- Phase 3.5: script→backend use_process round-trip ----
    // Allocate an input image in SHM, pass its handle through to the
    // script via RPC_TEST_SET_INPUT, then RUN. During inspect, the
    // script calls g_use_process which RPCs back here mid-call; our
    // handler doubles pixels and returns the output handle, which the
    // script stashes in its snapshot under "out_handle". We verify the
    // bytes — proves the full triangle (driver → runner script →
    // back to driver handler → script → reply).
    static xi_host_api host = xi::ImagePool::make_host_api();
    xi_image_handle in_h = host.shm_create_image(32, 16, 1);
    CHECK(in_h != 0);
    {
        uint8_t* px = host.image_data(in_h);
        for (int i = 0; i < 32 * 16; ++i) px[i] = (uint8_t)((i * 5 + 3) & 0x7F);
    }
    {
        ipc::Writer w; w.u64(in_h);
        auto r = rpc(ipc::RPC_TEST_SET_INPUT, w.buf());
        CHECK(r.type == (ipc::RPC_TEST_SET_INPUT | ipc::RPC_REPLY_BIT));
    }
    {
        ipc::Writer w; w.u32(99);
        auto r = rpc(ipc::RPC_SCRIPT_RUN, w.buf());
        CHECK(r.type == (ipc::RPC_SCRIPT_RUN | ipc::RPC_REPLY_BIT));
        ipc::Reader rd(r.payload);
        auto vars = rd.bytes();
        std::string vars_json(vars.begin(), vars.end());
        std::fprintf(stderr, "[test] phase3.5 vars: %s\n", vars_json.c_str());

        // Parse out_handle from the JSON.
        auto p = vars_json.find("\"out_handle\"");
        CHECK(p != std::string::npos);
        auto vp = vars_json.find("\"value\":", p);
        CHECK(vp != std::string::npos);
        uint64_t out_h = std::stoull(vars_json.substr(vp + 8));
        CHECK(out_h != 0);
        std::fprintf(stderr, "[test] script reported out_handle=0x%llx\n",
                     (unsigned long long)out_h);

        // Verify pixels are doubled. Both sides are looking at the
        // same SHM bytes — no copy.
        const uint8_t* op = host.image_data(out_h);
        int wrong = 0;
        for (int i = 0; i < 32 * 16; ++i) {
            int in_v = (i * 5 + 3) & 0x7F;
            int exp = in_v * 2; if (exp > 255) exp = 255;
            if (op[i] != (uint8_t)exp) ++wrong;
        }
        CHECK(wrong == 0);
        std::fprintf(stderr, "[test] phase3.5 use_process round-trip %s "
                              "(%d wrong of %d)\n",
                     wrong == 0 ? "ZERO-COPY OK" : "MISMATCH",
                     wrong, 32 * 16);
        host.image_release(out_h);

        // Phase 3.7 assertions — the script's inspect_entry also calls
        // exchange + grab; verify the outcomes landed in vars.
        //
        // SHM handles have 0xA5 in the top byte → values exceed int64
        // max, so use stoull throughout.
        auto find_num = [&](const std::string& key) -> uint64_t {
            auto p = vars_json.find("\"" + key + "\"");
            if (p == std::string::npos) return 0;
            auto vp = vars_json.find("\"value\":", p);
            if (vp == std::string::npos) return 0;
            try { return std::stoull(vars_json.substr(vp + 8)); }
            catch (...) { return 0; }
        };
        uint64_t exch_rc = find_num("exch_rc");
        uint64_t grabbed = find_num("grabbed_handle");
        std::fprintf(stderr,
            "[test] phase3.7 exch_rc=%llu grabbed=0x%llx\n",
            (unsigned long long)exch_rc, (unsigned long long)grabbed);
        CHECK(exch_rc > 0);
        CHECK(grabbed != 0);
        CHECK(vars_json.find("doubler") != std::string::npos);
        // Verify the grabbed image's bytes are 0x42 — proves zero-copy
        // path for grab too.
        const uint8_t* gp = host.image_data((xi_image_handle)grabbed);
        CHECK(gp != nullptr);
        bool all_42 = true;
        if (gp) {
            for (int i = 0; i < 64; ++i) if (gp[i] != 0x42) { all_42 = false; break; }
        }
        CHECK(all_42);
        if (grabbed) host.image_release((xi_image_handle)grabbed);
    }
    host.image_release(in_h);

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
