//
// test_sha256.cpp — verify against the standard FIPS 180-4 vectors.
//

#include <xi/xi_sha256.hpp>
#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(actual, expected)                                       \
    do {                                                              \
        if ((actual) != (expected)) {                                 \
            std::fprintf(stderr, "FAIL %s:%d\n  expected: %s\n  got:      %s\n", \
                __FILE__, __LINE__,                                   \
                std::string(expected).c_str(),                        \
                std::string(actual).c_str());                         \
            std::abort();                                             \
        }                                                             \
    } while (0)

int main() {
    using xi::sha256::sha256_bytes;

    // FIPS 180-4 known vectors.
    CHECK(sha256_bytes("", 0),
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    CHECK(sha256_bytes("abc", 3),
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    CHECK(sha256_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56),
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Long message: 1 000 000 'a' chars (multi-block path) cross-checked
    // against `python -c "import hashlib; print(hashlib.sha256(b'a'*1000000).hexdigest())"`.
    {
        std::string a(1'000'000, 'a');
        CHECK(sha256_bytes(a.data(), a.size()),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }
    // 64 + 128 byte 'a' messages — exercise exact-block-boundary
    // padding edge cases.
    {
        std::string a(64, 'a');
        CHECK(sha256_bytes(a.data(), a.size()),
              "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    }
    {
        std::string a(128, 'a');
        CHECK(sha256_bytes(a.data(), a.size()),
              "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e");
    }

    std::fprintf(stderr, "ALL TESTS PASSED\n");
    return 0;
}
