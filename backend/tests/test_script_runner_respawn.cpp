//
// test_script_runner_respawn.cpp — Tail 1 validation.
//
// Mirrors test_worker_respawn but for ScriptProcessAdapter. Kills the
// runner from outside; the next inspect_and_snapshot must transparently
// respawn, retry, and succeed. Rate limit caps at 3 in 60s.
//

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include <xi/xi_image_pool.hpp>
#include <xi/xi_script_process_adapter.hpp>
#include <xi/xi_shm.hpp>

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Find the runner's PID by walking the process tree from this process.
// ScriptProcessAdapter doesn't expose runner_pid() (yet), so we use
// the OS handle approximation: check our own children. Simpler — we'll
// scan for the most-recent xinsp-script-runner.exe.
static DWORD find_runner_pid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
    DWORD newest = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (lstrcmpiA(pe.szExeFile, "xinsp-script-runner.exe") == 0) {
                newest = pe.th32ProcessID;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return newest;
}

static bool kill_pid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    BOOL ok = TerminateProcess(h, 1);
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
    return ok != 0;
}

int main() {
    char self[MAX_PATH]; GetModuleFileNameA(nullptr, self, MAX_PATH);
    fs::path here       = fs::path(self).parent_path();
    fs::path runner_exe = here / "xinsp-script-runner.exe";
    fs::path script_dll = here / "test_script_runner_dll.dll";
    if (!fs::exists(runner_exe) || !fs::exists(script_dll)) return 2;

    char shm_name[64];
    std::snprintf(shm_name, sizeof(shm_name), "xinsp2-shm-srresp-%lu",
                  (unsigned long)GetCurrentProcessId());
    auto shm = xi::ShmRegion::create(shm_name, 8 * 1024 * 1024);
    xi::ImagePool::set_shm_region(&shm);

    xi::script::ScriptProcessAdapter ad;
    std::string err;
    CHECK(ad.start(runner_exe, script_dll, shm_name, err));
    DWORD pid_before = find_runner_pid();
    std::fprintf(stderr, "[test] initial runner pid≈%lu\n",
                 (unsigned long)pid_before);
    CHECK(pid_before != 0);

    // Pre-kill: a normal call works.
    {
        std::string vars;
        CHECK(ad.inspect_and_snapshot(7, vars, err));
        CHECK(vars.find("\"value\":14") != std::string::npos);
    }

    // ---- Respawn cycle 1 ----
    std::fprintf(stderr, "[test] killing runner pid=%lu...\n",
                 (unsigned long)pid_before);
    CHECK(kill_pid(pid_before));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    {
        std::string vars;
        bool ok = ad.inspect_and_snapshot(11, vars, err);
        CHECK(ok);
        if (!ok) std::fprintf(stderr, "[test] respawn-call err: %s\n", err.c_str());
        CHECK(vars.find("\"value\":22") != std::string::npos);
    }
    CHECK(ad.respawn_count() == 1);
    DWORD pid_after = find_runner_pid();
    CHECK(pid_after != 0);
    CHECK(pid_after != pid_before);
    std::fprintf(stderr, "[test] respawned: %lu → %lu\n",
                 (unsigned long)pid_before, (unsigned long)pid_after);

    // ---- Cycle to rate limit ----
    for (int i = 2; i <= 3; ++i) {
        DWORD pid = find_runner_pid();
        CHECK(kill_pid(pid));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::string vars;
        CHECK(ad.inspect_and_snapshot(i * 100, vars, err));
        CHECK(ad.respawn_count() == i);
    }
    // 4th kill — cap reached, RPC should fail and adapter goes dead.
    DWORD pid = find_runner_pid();
    CHECK(kill_pid(pid));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
        std::string vars;
        bool ok = ad.inspect_and_snapshot(99, vars, err);
        CHECK(!ok);
        CHECK(ad.is_dead());
    }
    std::fprintf(stderr, "[test] cap reached at %d respawns\n", ad.respawn_count());

    if (g_failures == 0) {
        std::fprintf(stderr, "test_script_runner_respawn: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_script_runner_respawn: %d FAIL\n", g_failures);
    return 1;
}
