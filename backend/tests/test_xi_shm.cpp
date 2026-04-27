//
// test_xi_shm.cpp — cross-process validation for xi::ShmRegion.
//
// Self-launching: with no args, runs the parent path that creates a
// region, allocates an image, fills it, then spawns ITSELF with
// `--child <region_name> <handle_hex> <expected_w> <expected_h>` to
// open + read the same region from a separate process.
//
// Validates:
//   1. Child process opens the named region successfully.
//   2. Child sees the EXACT same payload bytes the parent wrote
//      (no copy — both processes deref the same physical pages).
//   3. Cross-process atomic refcount: parent drops to 1, child addrefs
//      to 2, child releases to 1, parent releases to 0.
//   4. Bump allocator hands out distinct, valid handles for multiple
//      allocations from the same process.
//

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <xi/xi_shm.hpp>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Deterministic byte pattern so child can verify the parent's writes
// without coordinating actual content.
static uint8_t pattern_byte(int x, int y, int c) {
    return (uint8_t)((x * 7 + y * 31 + c * 113) & 0xFF);
}

// ---- CHILD PATH -------------------------------------------------------
// argv: <name> <handle_hex> <expected_w> <expected_h>
static int child_main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "child: missing args\n");
        return 99;
    }
    std::string name = argv[2];
    uint64_t handle = std::stoull(argv[3], nullptr, 16);
    int exp_w = std::atoi(argv[4]);
    int exp_h = std::atoi(argv[5]);

    try {
        auto rgn = xi::ShmRegion::attach(name);
        // Verify metadata round-trips through SHM
        if (rgn.width(handle)  != exp_w) { std::fprintf(stderr, "child: width mismatch\n"); return 11; }
        if (rgn.height(handle) != exp_h) { std::fprintf(stderr, "child: height mismatch\n"); return 12; }
        if (rgn.channels(handle) != 1)   { std::fprintf(stderr, "child: ch mismatch\n");    return 13; }

        // Verify payload byte-for-byte
        const uint8_t* p = rgn.data(handle);
        if (!p) { std::fprintf(stderr, "child: data() null\n"); return 14; }
        for (int y = 0; y < exp_h; ++y) {
            for (int x = 0; x < exp_w; ++x) {
                uint8_t expect = pattern_byte(x, y, 0);
                if (p[y * exp_w + x] != expect) {
                    std::fprintf(stderr, "child: pixel mismatch at (%d,%d): got %u expect %u\n",
                                 x, y, p[y * exp_w + x], expect);
                    return 15;
                }
            }
        }

        // Cross-process atomic refcount: addref then release.
        // Parent should observe both edits.
        rgn.addref(handle);
        int rc_after_addref = rgn.refcount(handle);
        std::fprintf(stderr, "child: refcount after addref = %d\n", rc_after_addref);
        rgn.release(handle);
        int rc_after_release = rgn.refcount(handle);
        std::fprintf(stderr, "child: refcount after release = %d\n", rc_after_release);

        // Allocate a small block from the child side too; parent will
        // verify block_count went up.
        uint64_t cb = rgn.alloc_buffer(128);
        if (cb == xi::ShmRegion::INVALID_HANDLE) { std::fprintf(stderr, "child: alloc failed\n"); return 16; }
        // Stamp a sentinel so parent can read it
        std::memset(rgn.data(cb), 0xCD, 128);
        std::fprintf(stderr, "child: alloc'd buffer handle=0x%llx\n",
                     (unsigned long long)cb);
        // Print on stdout so parent can pick it up
        std::printf("CHILD_BUFFER_HANDLE=%llx\n", (unsigned long long)cb);
        std::fflush(stdout);

        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "child: exception: %s\n", e.what());
        return 1;
    }
}

#ifdef _WIN32
// Spawn `argv[0] --child <args>` and capture stdout. Returns exit code.
struct ChildResult { int exit_code; std::string stdout_text; };
static ChildResult run_child(const std::string& exe, const std::string& args) {
    HANDLE rd, wr;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return { -1, "" };
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + exe + "\" " + args;
    std::vector<char> cmdbuf(cmd.begin(), cmd.end()); cmdbuf.push_back(0);
    if (!CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE, 0,
                        nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return { -1, "" };
    }
    CloseHandle(wr);

    std::string out;
    char buf[1024];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
        out.append(buf, n);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return { (int)code, out };
}
#endif

// ---- PARENT PATH ------------------------------------------------------
static int parent_main(const char* exe_path) {
    // Unique region name per run so concurrent test invocations don't
    // collide on the same OS object.
    char name[64];
    std::snprintf(name, sizeof(name), "xinsp2-shm-test-%lu", (unsigned long)GetCurrentProcessId());

    try {
        auto rgn = xi::ShmRegion::create(name, 16 * 1024 * 1024);
        std::fprintf(stderr, "parent: created region '%s' size=%llu\n",
                     name, (unsigned long long)rgn.total_size());

        // Allocate a 64x32 image, fill with the deterministic pattern.
        const int W = 64, H = 32;
        uint64_t h_img = rgn.alloc_image(W, H, 1);
        CHECK(h_img != xi::ShmRegion::INVALID_HANDLE);
        CHECK(rgn.refcount(h_img) == 1);
        CHECK(rgn.width(h_img)    == W);
        CHECK(rgn.height(h_img)   == H);
        CHECK(rgn.channels(h_img) == 1);

        uint8_t* px = rgn.data(h_img);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                px[y * W + x] = pattern_byte(x, y, 0);

        // Allocate a second block to verify distinct handles + bump
        // allocator working in this process.
        uint64_t h_buf = rgn.alloc_buffer(2048);
        CHECK(h_buf != xi::ShmRegion::INVALID_HANDLE);
        CHECK(h_buf != h_img);
        CHECK(rgn.size(h_buf) == 2048);
        CHECK(rgn.block_count() == 2);

        std::fprintf(stderr, "parent: handles img=0x%llx buf=0x%llx, used=%llu\n",
                     (unsigned long long)h_img, (unsigned long long)h_buf,
                     (unsigned long long)rgn.used_bytes());

        // Spawn child with --child <name> <handle_hex> <w> <h>
        char args[256];
        std::snprintf(args, sizeof(args), "--child %s %llx %d %d",
                      name, (unsigned long long)h_img, W, H);
#ifdef _WIN32
        ChildResult cr = run_child(exe_path, args);
        std::fprintf(stderr, "parent: child exit=%d, stdout=[%s]\n",
                     cr.exit_code, cr.stdout_text.c_str());
        CHECK(cr.exit_code == 0);
#else
        (void)exe_path; (void)args;
        std::fprintf(stderr, "parent: child spawn only on Windows in this spike\n");
#endif

        // After child returns:
        //  - It addref'd then released, so net 0 from child on h_img.
        //  - Parent still holds 1 ref from alloc_image.
        CHECK(rgn.refcount(h_img) == 1);

        // Child also alloc'd a 128-byte buffer. Verify block_count
        // bumped to 3 and parse the handle from child stdout.
#ifdef _WIN32
        CHECK(rgn.block_count() == 3);
        const std::string tag = "CHILD_BUFFER_HANDLE=";
        auto p = cr.stdout_text.find(tag);
        if (p != std::string::npos) {
            std::string hex = cr.stdout_text.substr(p + tag.size());
            // strip trailing whitespace
            while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' ||
                                    hex.back() == ' '  || hex.back() == '\t')) hex.pop_back();
            uint64_t cb = std::stoull(hex, nullptr, 16);
            // Cross-process visibility: parent sees child's writes.
            const uint8_t* cp = rgn.data(cb);
            CHECK(cp != nullptr);
            if (cp) {
                bool all_cd = true;
                for (int i = 0; i < 128; ++i) if (cp[i] != 0xCD) { all_cd = false; break; }
                CHECK(all_cd);
            }
            // Drop the child's allocated block to ref 0.
            rgn.release(cb);
        } else {
            CHECK(false /* child did not emit CHILD_BUFFER_HANDLE */);
        }
#endif

        // Parent drops its ref → 0.
        int rc = rgn.release(h_img);
        CHECK(rc == 0);

        // Allocate something else to verify the bump allocator still
        // works after refcount churn (it's bump-only, no reclaim).
        uint64_t h_extra = rgn.alloc_image(8, 8, 1);
        CHECK(h_extra != xi::ShmRegion::INVALID_HANDLE);
        CHECK(h_extra != h_img);
        rgn.release(h_extra);

        // Bad handle path: stale tag should fail validation.
        uint64_t bogus = 0xFFull << 56;  // wrong tag
        CHECK(rgn.data(bogus) == nullptr);
        CHECK(rgn.refcount(bogus) == -1);

        if (g_failures == 0) {
            std::fprintf(stderr, "test_xi_shm: ALL PASS\n");
            return 0;
        } else {
            std::fprintf(stderr, "test_xi_shm: %d FAIL\n", g_failures);
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "parent: exception: %s\n", e.what());
        return 2;
    }
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--child") {
        return child_main(argc, argv);
    }
    return parent_main(argv[0]);
}
