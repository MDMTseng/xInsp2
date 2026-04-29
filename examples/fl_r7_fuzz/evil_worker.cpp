// evil_worker.cpp — FL r7 target #3 fuzzer counterpart.
//
// Drops in as `xinsp-worker.exe` (the harness places this binary at
// the host-expected path). The host spawns us with:
//   --pipe=<name> --shm=<name> --plugin-dll=<path> --instance=<name>
// We connect to the named pipe (host is the server) and inject
// adversarial frames into it. The host's reader thread in
// xi_process_instance.hpp must survive: kill the worker cleanly OR
// exit the reader cleanly, but the host process must remain
// responsive.
//
// Per project policy: gate platform calls + leave a TODO(linux) note.
// xi_ipc.hpp already #ifdef's _WIN32; this binary won't link on Linux
// today.
//
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <windows.h>
#else
// TODO(linux): port this fuzzer once xi_ipc.hpp grows a POSIX backend
//              (currently the header is Win32-only; see line "throw
//              runtime_error('xi_ipc Windows-only in this spike')").
#error "evil_worker is Windows-only at present"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

constexpr uint32_t FRAME_MAGIC    = 0x49504958u; // 'XIPI'
constexpr uint32_t RPC_REPLY_BIT  = 0x80000000u;
constexpr uint32_t RPC_TYPE_ERROR = 0xFFu;

struct FrameHeader {
    uint32_t magic;
    uint32_t seq;
    uint32_t type;
    uint32_t len;
};

static std::string get_arg(int argc, char** argv, const char* prefix) {
    size_t plen = std::strlen(prefix);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], prefix, plen) == 0) {
            return std::string(argv[i] + plen);
        }
    }
    return {};
}

static HANDLE connect_pipe(const std::string& name, int timeout_ms) {
    std::string full = R"(\\.\pipe\)" + name;
    DWORD start = GetTickCount();
    for (;;) {
        HANDLE h = CreateFileA(full.c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
            std::fprintf(stderr, "[evil_worker] CreateFileA err=%lu\n", err);
            return INVALID_HANDLE_VALUE;
        }
        if ((int)(GetTickCount() - start) > timeout_ms) return INVALID_HANDLE_VALUE;
        Sleep(20);
    }
}

static bool write_all(HANDLE h, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    while (n) {
        DWORD wrote = 0;
        if (!WriteFile(h, p, (DWORD)n, &wrote, nullptr)) return false;
        if (wrote == 0) return false;
        p += wrote; n -= wrote;
    }
    return true;
}

static void send_raw_frame(HANDLE h, uint32_t magic, uint32_t seq,
                           uint32_t type, uint32_t len,
                           const void* payload, size_t payload_actual) {
    FrameHeader hdr{ magic, seq, type, len };
    write_all(h, &hdr, sizeof(hdr));
    if (payload_actual) write_all(h, payload, payload_actual);
}

// Returns the strategy name actually used (for the harness to read
// from stdout — Python driver consumes this).
static const char* fuzz_one(HANDLE pipe, std::mt19937& rng, int strat_idx) {
    static const char* STRATS[] = {
        "bad_magic", "huge_len", "len_zero_no_payload",
        "len_4_no_payload", "opcode_ffff", "partial_header",
        "len_mismatch_short", "len_mismatch_long", "negative_len",
        "garbage_then_close", "many_zero_frames", "emit_trigger_garbage",
        "emit_trigger_huge", "emit_trigger_neg_count", "create_garbage",
        "process_bogus_handle",
    };
    constexpr int N = (int)(sizeof(STRATS) / sizeof(STRATS[0]));
    int s = strat_idx % N;
    const char* name = STRATS[s];

    std::vector<uint8_t> payload;
    auto fillrand = [&](size_t n){
        payload.resize(n);
        for (auto& b : payload) b = (uint8_t)(rng() & 0xFF);
    };

    switch (s) {
    case 0: { // bad_magic
        fillrand(rng() % 32);
        send_raw_frame(pipe, 0xDEADBEEF, 1, 1, (uint32_t)payload.size(),
                       payload.data(), payload.size());
        break;
    }
    case 1: { // huge_len — claim 2GB but send nothing
        send_raw_frame(pipe, FRAME_MAGIC, 2, 3, 0x7FFFFFFFu, nullptr, 0);
        break;
    }
    case 2: { // len_zero_no_payload — valid framing, empty
        send_raw_frame(pipe, FRAME_MAGIC, 3, 0xFFFF, 0, nullptr, 0);
        break;
    }
    case 3: { // len=4 no payload — header but no body, half-frame
        FrameHeader hdr{ FRAME_MAGIC, 4, 3, 4 };
        write_all(pipe, &hdr, sizeof(hdr));
        // and then close
        Sleep(50);
        return name;
    }
    case 4: { // opcode_ffff
        fillrand(8);
        send_raw_frame(pipe, FRAME_MAGIC, 5, 0xFFFFu, (uint32_t)payload.size(),
                       payload.data(), payload.size());
        break;
    }
    case 5: { // partial_header — only 8 bytes then close
        uint8_t buf[8] = { 0x58,0x49,0x50,0x49, 0,0,0,0 };
        write_all(pipe, buf, sizeof(buf));
        Sleep(50);
        return name;
    }
    case 6: { // len_mismatch_short — claim 32, send 4
        FrameHeader hdr{ FRAME_MAGIC, 6, 3, 32 };
        write_all(pipe, &hdr, sizeof(hdr));
        uint8_t junk[4] = { 1,2,3,4 };
        write_all(pipe, junk, 4);
        Sleep(50);
        return name;
    }
    case 7: { // len_mismatch_long — claim 4, send 32
        send_raw_frame(pipe, FRAME_MAGIC, 7, 3, 4, nullptr, 0);
        std::vector<uint8_t> junk(32, 0xAA);
        write_all(pipe, junk.data(), junk.size());
        break;
    }
    case 8: { // negative_len — uint32 0xFFFFFFFF (==-1 signed) — > MAX_PAYLOAD
        send_raw_frame(pipe, FRAME_MAGIC, 8, 3, 0xFFFFFFFFu, nullptr, 0);
        break;
    }
    case 9: { // garbage_then_close
        fillrand(rng() % 256);
        write_all(pipe, payload.data(), payload.size());
        Sleep(20);
        return name;
    }
    case 10: { // many_zero frames — flood with magic-only empty frames
        for (int i = 0; i < 100; ++i) {
            send_raw_frame(pipe, FRAME_MAGIC, (uint32_t)i, 0, 0, nullptr, 0);
        }
        break;
    }
    case 11: { // emit_trigger garbage payload
        fillrand(rng() % 4096);
        send_raw_frame(pipe, FRAME_MAGIC, 0 /*seq=0 emit*/, 12 /*RPC_EMIT_TRIGGER*/,
                       (uint32_t)payload.size(), payload.data(), payload.size());
        break;
    }
    case 12: { // emit_trigger huge but valid-prefix
        // u32 source_name_len + src + u64 tid_hi + u64 tid_lo + i64 ts + u32 image_count + ...
        // Set image_count to 2^31 - hostile
        std::vector<uint8_t> p;
        auto u32 = [&](uint32_t v){ p.insert(p.end(), (uint8_t*)&v, (uint8_t*)&v + 4); };
        auto u64 = [&](uint64_t v){ p.insert(p.end(), (uint8_t*)&v, (uint8_t*)&v + 8); };
        const char* src = "evil_src";
        u32((uint32_t)std::strlen(src));
        p.insert(p.end(), src, src + std::strlen(src));
        u64(0); u64(1);
        u64(123456789ULL);
        u32(0x7FFFFFFFu);  // huge image_count
        send_raw_frame(pipe, FRAME_MAGIC, 0, 12, (uint32_t)p.size(), p.data(), p.size());
        break;
    }
    case 13: { // emit_trigger negative count via uint32 max
        std::vector<uint8_t> p;
        auto u32 = [&](uint32_t v){ p.insert(p.end(), (uint8_t*)&v, (uint8_t*)&v + 4); };
        u32(0xFFFFFFFFu); // source_name_len overflow
        send_raw_frame(pipe, FRAME_MAGIC, 0, 12, (uint32_t)p.size(), p.data(), p.size());
        break;
    }
    case 14: { // CREATE garbage — looks like RPC_CREATE but with bogus name_len
        std::vector<uint8_t> p;
        auto u32 = [&](uint32_t v){ p.insert(p.end(), (uint8_t*)&v, (uint8_t*)&v + 4); };
        u32(0x7FFFFFFFu); // name_len bigger than payload
        send_raw_frame(pipe, FRAME_MAGIC, 99, 1 /*RPC_CREATE*/,
                       (uint32_t)p.size(), p.data(), p.size());
        break;
    }
    case 15: { // process bogus handle — RPC_REPLY for a non-existent seq
        std::vector<uint8_t> p(12, 0);
        send_raw_frame(pipe, FRAME_MAGIC, 0xDEAD,
                       3 | RPC_REPLY_BIT, (uint32_t)p.size(),
                       p.data(), p.size());
        break;
    }
    default: break;
    }
    return name;
}

int main(int argc, char** argv) {
    std::string pipe_name = get_arg(argc, argv, "--pipe=");
    if (pipe_name.empty()) {
        std::fprintf(stderr, "[evil_worker] no --pipe= arg\n");
        return 2;
    }

    // Optional FUZZ_STRATEGY env var to pin one strategy; otherwise random.
    int strat_pin = -1;
    if (const char* s = std::getenv("FUZZ_STRATEGY")) {
        try { strat_pin = std::atoi(s); } catch (...) { strat_pin = -1; }
    }

    // Random seed: env or time-based
    unsigned seed = (unsigned)GetTickCount();
    if (const char* s = std::getenv("FUZZ_SEED")) {
        try { seed = (unsigned)std::stoul(s); } catch (...) {}
    }
    std::mt19937 rng(seed);

    std::fprintf(stderr, "[evil_worker] connecting pipe=%s seed=%u strat_pin=%d\n",
                 pipe_name.c_str(), seed, strat_pin);

    HANDLE pipe = connect_pipe(pipe_name, 5000);
    if (pipe == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "[evil_worker] could not connect pipe\n");
        return 3;
    }

    // Special strategy_pin -1: connect and IMMEDIATELY close (no
    // frames at all). Used as a control to see if the host crashes
    // from EOF alone vs from any specific malformed frame.
    if (strat_pin == -2) {
        std::fprintf(stderr, "[evil_worker] strategy=immediate_close\n");
        std::printf("STRATEGY=immediate_close\n");
        std::fflush(stdout);
        Sleep(50);
        CloseHandle(pipe);
        return 0;
    }

    int idx = (strat_pin >= 0) ? strat_pin : (int)(rng() % 16);
    const char* used = fuzz_one(pipe, rng, idx);
    std::fprintf(stderr, "[evil_worker] sent strategy=%s (idx=%d)\n", used, idx);
    // Print on stdout for the harness to scrape.
    std::printf("STRATEGY=%s\n", used);
    std::fflush(stdout);

    // Hold the pipe a moment, then close. Closing an in-flight pipe is
    // also a fuzz vector — host's reader hits EOF mid-frame.
    Sleep(200);
    CloseHandle(pipe);
    return 0;
}
