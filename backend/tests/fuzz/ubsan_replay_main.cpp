// ubsan_replay_main.cpp — a standalone driver (no libFuzzer) that replays a
// corpus through a harness's LLVMFuzzerTestOneInput under UndefinedBehaviorSanitizer.
//
// Why: on Windows the prebuilt clang_rt.fuzzer lib links cleanly with ASan but
// hits a CRT (_stricmp dllimport) mismatch when combined with UBSan alone. To
// still exercise the exact fuzzed boundaries under UBSan, we compile each harness
// TU with -fsanitize=undefined and link it against THIS main instead of the
// libFuzzer runtime. It walks every file given on argv (or in a given directory)
// and feeds its bytes to the target once. Point it at the corpus the ASan fuzzer
// grew for maximum path coverage.
//
// UBSan is configured with halt_on_error=1 (see the ctest ENVIRONMENT), so any
// signed-overflow / shift / invalid-cast / OOB the harness triggers aborts here.
//
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace fs = std::filesystem;

static int run_file(const fs::path& p) {
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return 0;
    std::vector<uint8_t> buf;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        buf.resize((size_t)n);
        size_t got = std::fread(buf.data(), 1, buf.size(), f);
        buf.resize(got);
    }
    std::fclose(f);
    LLVMFuzzerTestOneInput(buf.data(), buf.size());
    return 1;
}

int main(int argc, char** argv) {
    // Always feed the empty input first (edge case).
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(""), 0);

    int count = 0;
    for (int i = 1; i < argc; ++i) {
        fs::path p(argv[i]);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            for (auto& e : fs::recursive_directory_iterator(p, ec)) {
                if (e.is_regular_file()) count += run_file(e.path());
            }
        } else if (fs::is_regular_file(p, ec)) {
            count += run_file(p);
        }
    }
    std::printf("[ubsan-replay] %s: replayed %d input(s), no UBSan error\n",
                argc > 0 ? argv[0] : "?", count);
    return 0;
}
