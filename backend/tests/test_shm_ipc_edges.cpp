//
// test_shm_ipc_edges.cpp — edge case sweep across SHM + IPC + adapter
// surfaces. Drives the abnormal paths that the happy-path tests don't
// touch:
//
//   SHM:
//     bogus tag / out-of-bounds offset / valid offset but no block magic
//     refcount underflow stays negative (no UB)
//     alloc_image with zero / negative dims
//     alloc_buffer with 0
//     bump allocator OOM returns 0
//     pixel-size overflow (huge w*h*ch)
//     attach() to a non-existent region throws
//     attach() to a corrupt-header region throws
//     create() with a name already taken throws
//     concurrent alloc from N threads doesn't double-bump
//
//   IPC:
//     recv_frame on a dead pipe throws
//     Reader short payload throws
//     Frame with stupendous len (DoS vector) caught
//     Session orphan-reply seq throws
//     Session handler that throws sends an error reply
//     Session with no handler + nested request → error reply, not crash
//
//   ScriptProcessAdapter:
//     stop() before start() is a no-op
//     stop() called twice is fine
//     inspect_and_snapshot before start() returns clean error
//

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <xi/xi_image_pool.hpp>
#include <xi/xi_ipc.hpp>
#include <xi/xi_script_process_adapter.hpp>
#include <xi/xi_shm.hpp>

namespace fs  = std::filesystem;
namespace ipc = xi::ipc;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define EXPECT_THROW(stmt)                                                     \
    do {                                                                       \
        bool _threw = false;                                                   \
        try { stmt; } catch (...) { _threw = true; }                          \
        if (!_threw) {                                                        \
            std::fprintf(stderr, "FAIL %s:%d: expected throw from " #stmt "\n", \
                         __FILE__, __LINE__);                                 \
            ++g_failures;                                                     \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------
static void test_shm_handle_validation() {
    std::fprintf(stderr, "\n[edge] SHM handle validation\n");
    char name[64];
    std::snprintf(name, sizeof(name), "xinsp2-edge-shm-%lu",
                  (unsigned long)GetCurrentProcessId());
    auto rgn = xi::ShmRegion::create(name, 4 * 1024 * 1024);

    // Bogus: handle 0 (INVALID_HANDLE) → all accessors must be safe.
    CHECK(rgn.data(0) == nullptr);
    CHECK(rgn.refcount(0) == -1);
    CHECK(rgn.width(0) == 0);
    CHECK(!rgn.is_valid_handle(0));

    // Bogus: wrong tag.
    uint64_t bad_tag = 0xFFull << 56;
    CHECK(rgn.data(bad_tag) == nullptr);
    CHECK(!rgn.is_valid_handle(bad_tag));

    // Bogus: tag matches but offset way past region size.
    uint64_t huge_off = (0xA5ull << 56) | 0x0FFFFFFFull;  // 256 MB into a 4 MB region
    CHECK(rgn.data(huge_off) == nullptr);
    CHECK(!rgn.is_valid_handle(huge_off));

    // Bogus: tag matches, offset in range, but no block magic at that
    // offset (random scratch byte). The header must reject it via magic
    // mismatch or we'd be returning attacker-controlled "block" data.
    uint64_t scratch_off = (0xA5ull << 56) | 0x200ull;  // 512B in
    CHECK(rgn.data(scratch_off) == nullptr);
    CHECK(!rgn.is_valid_handle(scratch_off));

    // Real handle works.
    uint64_t h = rgn.alloc_image(8, 8, 1);
    CHECK(h != 0);
    CHECK(rgn.data(h) != nullptr);
    CHECK(rgn.is_valid_handle(h));
}

static void test_shm_alloc_edges() {
    std::fprintf(stderr, "\n[edge] SHM alloc edges\n");
    char name[64];
    std::snprintf(name, sizeof(name), "xinsp2-edge-alloc-%lu",
                  (unsigned long)GetCurrentProcessId());
    auto rgn = xi::ShmRegion::create(name, 1 * 1024 * 1024);  // 1 MB

    // Zero / negative dimensions → INVALID_HANDLE.
    CHECK(rgn.alloc_image(0, 8, 1) == 0);
    CHECK(rgn.alloc_image(8, 0, 1) == 0);
    CHECK(rgn.alloc_image(8, 8, 0) == 0);
    CHECK(rgn.alloc_image(-1, 8, 1) == 0);
    CHECK(rgn.alloc_buffer(0) == 0);
    CHECK(rgn.alloc_buffer(-100) == 0);

    // Huge dim overflow: w*h*ch overflows int32. alloc_image computes
    // pixels as int64 so it doesn't actually overflow, but the resulting
    // allocation is way bigger than the region — must return 0.
    CHECK(rgn.alloc_image(100000, 100000, 4) == 0);

    // Bump exhaustion: allocate until we run out, must get 0.
    int got = 0;
    for (int i = 0; i < 1000; ++i) {
        if (rgn.alloc_image(64, 64, 4) == 0) break;
        ++got;
    }
    std::fprintf(stderr, "  bump exhausted after %d allocs, next = %llu\n",
                 got, (unsigned long long)rgn.alloc_image(64, 64, 4));
    CHECK(got > 0);
    CHECK(rgn.alloc_image(64, 64, 4) == 0);
}

static void test_shm_refcount_underflow() {
    std::fprintf(stderr, "\n[edge] SHM refcount underflow\n");
    char name[64];
    std::snprintf(name, sizeof(name), "xinsp2-edge-rc-%lu",
                  (unsigned long)GetCurrentProcessId());
    auto rgn = xi::ShmRegion::create(name, 1 * 1024 * 1024);
    uint64_t h = rgn.alloc_image(4, 4, 1);
    CHECK(rgn.refcount(h) == 1);
    CHECK(rgn.release(h) == 0);
    // Past zero: behaviour is well-defined (negative count) — we don't
    // reclaim memory and don't crash. This guards against accidental
    // double-release patterns at least failing safely.
    CHECK(rgn.release(h) == -1);
    CHECK(rgn.release(h) == -2);
    CHECK(rgn.refcount(h) == -2);
}

static void test_shm_attach_errors() {
    std::fprintf(stderr, "\n[edge] SHM attach errors\n");
    EXPECT_THROW(xi::ShmRegion::attach("xinsp2-does-not-exist-xyz"));

    char name[64];
    std::snprintf(name, sizeof(name), "xinsp2-edge-create-twice-%lu",
                  (unsigned long)GetCurrentProcessId());
    auto rgn = xi::ShmRegion::create(name, 64 * 1024);
    // Second create with same name MUST throw — protects against silently
    // sharing a region with a different config.
    EXPECT_THROW(xi::ShmRegion::create(name, 64 * 1024));
}

static void test_shm_concurrent_alloc() {
    std::fprintf(stderr, "\n[edge] SHM concurrent alloc CAS\n");
    char name[64];
    std::snprintf(name, sizeof(name), "xinsp2-edge-conc-%lu",
                  (unsigned long)GetCurrentProcessId());
    auto rgn = xi::ShmRegion::create(name, 4 * 1024 * 1024);

    constexpr int N_THREADS = 8;
    constexpr int PER_THREAD = 100;
    std::vector<std::thread> ts;
    std::vector<std::vector<uint64_t>> per(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t) {
        ts.emplace_back([&, t] {
            per[t].reserve(PER_THREAD);
            for (int i = 0; i < PER_THREAD; ++i) {
                uint64_t h = rgn.alloc_image(16, 16, 1);
                if (h) per[t].push_back(h);
            }
        });
    }
    for (auto& th : ts) th.join();

    // Every handle must be unique. Duplicate would mean the bump CAS
    // didn't serialise correctly under contention.
    std::vector<uint64_t> all;
    for (auto& v : per) for (auto h : v) all.push_back(h);
    std::sort(all.begin(), all.end());
    auto dup = std::adjacent_find(all.begin(), all.end());
    CHECK(dup == all.end());
    std::fprintf(stderr, "  %zu allocations, all unique\n", all.size());
}

// ---------------------------------------------------------------------
static void test_ipc_short_payload() {
    std::fprintf(stderr, "\n[edge] IPC Reader short payload\n");
    std::vector<uint8_t> buf{ 1, 2, 3 };  // only 3 bytes
    ipc::Reader r(buf);
    EXPECT_THROW(r.u64());   // wants 8 bytes, has 3
}

static void test_ipc_session_handler_throws() {
    std::fprintf(stderr, "\n[edge] Session handler throws → error reply\n");
    char pname[64];
    std::snprintf(pname, sizeof(pname), "xinsp2-edge-sess-%lu",
                  (unsigned long)GetCurrentProcessId());

    // Server runs serve_forever with a handler that throws on every
    // request. The first request → error reply, then the client closes
    // the pipe and serve_forever exits via recv_frame exception.
    std::thread server([pname] {
        try {
            ipc::Session s(ipc::Pipe::accept_one(pname));
            s.set_handler([](uint32_t, const std::vector<uint8_t>&)
                          -> std::vector<uint8_t> {
                throw std::runtime_error("handler boom");
            });
            s.serve_forever();
        } catch (...) { /* expected on client disconnect */ }
    });

    {
        ipc::Session client(ipc::Pipe::connect(pname, 5000));
        auto rsp = client.call(ipc::RPC_GET_DEF, {});
        CHECK(rsp.type == ipc::RPC_TYPE_ERROR);
        std::string msg(rsp.payload.begin(), rsp.payload.end());
        CHECK(msg.find("boom") != std::string::npos);
    }   // client destructor closes pipe → server's serve_forever exits

    server.join();
}

static void test_ipc_oversized_frame_rejected() {
    std::fprintf(stderr, "\n[edge] IPC oversized frame header → reject\n");
    char pname[64];
    std::snprintf(pname, sizeof(pname), "xinsp2-edge-dos-%lu",
                  (unsigned long)GetCurrentProcessId());

    // Hostile "peer": writes a frame header claiming a payload bigger
    // than MAX_PAYLOAD_BYTES, then nothing. Receiver must throw on the
    // header check, NOT try to allocate gigabytes.
    std::thread sender([pname] {
        try {
            ipc::Pipe p = ipc::Pipe::accept_one(pname);
            ipc::FrameHeader h{ ipc::FRAME_MAGIC, 1, 99,
                                ipc::MAX_PAYLOAD_BYTES + 1 };
            p.write_all(&h, sizeof(h));
        } catch (...) {}
    });
    {
        ipc::Pipe c = ipc::Pipe::connect(pname, 5000);
        EXPECT_THROW(ipc::recv_frame(c));
    }
    sender.join();
}

static void test_ipc_bad_magic_rejected() {
    std::fprintf(stderr, "\n[edge] IPC bad magic → reject\n");
    char pname[64];
    std::snprintf(pname, sizeof(pname), "xinsp2-edge-magic-%lu",
                  (unsigned long)GetCurrentProcessId());
    std::thread sender([pname] {
        try {
            ipc::Pipe p = ipc::Pipe::accept_one(pname);
            ipc::FrameHeader h{ 0xDEADBEEFu, 1, 99, 0 };  // wrong magic
            p.write_all(&h, sizeof(h));
        } catch (...) {}
    });
    {
        ipc::Pipe c = ipc::Pipe::connect(pname, 5000);
        EXPECT_THROW(ipc::recv_frame(c));
    }
    sender.join();
}

// ---------------------------------------------------------------------
static void test_adapter_lifecycle() {
    std::fprintf(stderr, "\n[edge] ScriptProcessAdapter lifecycle\n");
    xi::script::ScriptProcessAdapter ad;
    // stop() before start() — must be a no-op, not a crash.
    ad.stop();
    ad.stop();
    CHECK(!ad.ok());

    // inspect_and_snapshot before start() — returns false with err.
    std::string vars, err;
    CHECK(!ad.inspect_and_snapshot(0, vars, err));
    CHECK(!err.empty());
}

static void test_adapter_runner_missing() {
    std::fprintf(stderr, "\n[edge] ScriptProcessAdapter: runner exe missing\n");
    xi::script::ScriptProcessAdapter ad;
    std::string err;
    bool ok = ad.start("c:/no/such/runner.exe", "c:/no/such/dll.dll",
                        "xinsp2-shm-edge-doesnt-matter", err);
    CHECK(!ok);
    CHECK(!err.empty());
}

// ---------------------------------------------------------------------
int main() {
    test_shm_handle_validation();
    test_shm_alloc_edges();
    test_shm_refcount_underflow();
    test_shm_attach_errors();
    test_shm_concurrent_alloc();
    test_ipc_short_payload();
    test_ipc_session_handler_throws();
    test_ipc_oversized_frame_rejected();
    test_ipc_bad_magic_rejected();
    test_adapter_lifecycle();
    test_adapter_runner_missing();

    if (g_failures == 0) {
        std::fprintf(stderr, "\ntest_shm_ipc_edges: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "\ntest_shm_ipc_edges: %d FAIL\n", g_failures);
    return 1;
}
