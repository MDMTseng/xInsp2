// fuzz_parse_cmd.cpp — coverage-guided fuzz target for the WS command parser.
//
// Boundary under test: xi::proto::parse_cmd() and the sibling field pluckers
// (get_string_field / get_number_field) in xi_protocol.hpp. This is the
// hand-rolled, dependency-free JSON cursor the backend runs on EVERY inbound
// WebSocket `cmd` frame (see service_main.cpp:~1910). The Python black-box WS
// smoke can only reach this through a live socket; here we hit it in-process at
// millions of execs, feeding arbitrary bytes straight into the parser — the
// exact adversary a malicious/broken client is.
//
// The parser walks raw pointers (skip_ws / extract_value / find_key) and does
// its own \uXXXX + UTF-16 surrogate decoding in strip_quotes — prime territory
// for OOB reads on truncated escapes, unbalanced braces, or lone surrogates.
//
// Build (clang-cl): see backend/tests/fuzz/README.md. Header included READ-ONLY.
//
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "xi/xi_protocol.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view sv(reinterpret_cast<const char*>(data), size);

    // 1) The primary entry: full cmd-frame parse.
    if (auto cmd = xi::proto::parse_cmd(sv)) {
        // Touch the results so the optimizer can't elide the decode work.
        volatile int64_t id = cmd->id;
        (void)id;
        volatile size_t n = cmd->name.size() + cmd->args_json.size();
        (void)n;
    }

    // 2) The field pluckers share the same cursor/unescape machinery and are
    //    called by individual command handlers on args objects. Fuzz them with
    //    a couple of representative keys so the escape/hex paths get exercised
    //    even when parse_cmd bails early.
    if (auto s = xi::proto::get_string_field(sv, "path")) {
        volatile size_t n = s->size();
        (void)n;
    }
    if (auto d = xi::proto::get_number_field(sv, "id")) {
        volatile double v = *d;
        (void)v;
    }
    return 0;
}
