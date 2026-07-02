//
// test_protocol.cpp — M1 regression test for xi::proto.
//
// Exercises encode/decode round-trips for every message type, plus the
// preview binary header, plus a parse of the fixture file the TS side
// also parses.
//

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <xi/xi_protocol.hpp>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define SECTION(name) std::printf("[proto] %s\n", name)

using namespace xi::proto;

static void test_cmd_encode() {
    SECTION("Cmd::to_json");
    Cmd c;
    c.id = 42;
    c.name = "run";
    c.args_json = R"({"frame_path":"C:/images/sample.bmp"})";
    std::string s = c.to_json();
    CHECK(s.find("\"type\":\"cmd\"")       != std::string::npos);
    CHECK(s.find("\"id\":42")              != std::string::npos);
    CHECK(s.find("\"name\":\"run\"")       != std::string::npos);
    CHECK(s.find("\"frame_path\"")         != std::string::npos);
}

static void test_cmd_parse_fixture() {
    SECTION("parse_cmd on fixture");
    // Path is relative to the build working directory — the CMake test
    // registers an env var so we can find the source tree.
    const char* fixtures = std::getenv("XINSP2_FIXTURES");
    std::string path = std::string(fixtures ? fixtures : "../../protocol/fixtures")
                     + "/cmd_run.json";
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        ++g_failures;
        return;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    std::string text = buf.str();

    auto parsed = parse_cmd(text);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK(parsed->id == 1);
        CHECK(parsed->name == "run");
        auto fp = get_string_field(parsed->args_json, "frame_path");
        CHECK(fp.has_value());
        if (fp) CHECK(*fp == "C:/images/sample.bmp");
    }
}

static void test_rsp_ok() {
    SECTION("Rsp ok with data");
    Rsp r;
    r.id = 7;
    r.ok = true;
    r.data_json = R"({"pong":true})";
    std::string s = r.to_json();
    CHECK(s.find("\"ok\":true") != std::string::npos);
    CHECK(s.find("\"data\":{\"pong\":true}") != std::string::npos);
}

static void test_rsp_err() {
    SECTION("Rsp err");
    Rsp r;
    r.id = 7;
    r.ok = false;
    r.error = "unknown command: xyz";
    std::string s = r.to_json();
    CHECK(s.find("\"ok\":false") != std::string::npos);
    CHECK(s.find("\"error\":\"unknown command: xyz\"") != std::string::npos);
}

// (test_vars_encode removed: the `vars` wire — Vars/VarItem/VarKindWire — was
// deleted with the v9 vars/value-store teardown; script output is now the
// `expose` plugin's XEX1 binary frame, not a core protocol type.)

static void test_log_event() {
    SECTION("Log + Event");
    LogMsg lm;
    lm.level = "info";
    lm.msg = "compile ok";
    std::string s1 = lm.to_json();
    CHECK(s1.find("\"type\":\"log\"") != std::string::npos);
    CHECK(s1.find("\"level\":\"info\"") != std::string::npos);

    Event e;
    e.name = "run_finished";
    e.data_json = R"({"ms":42})";
    std::string s2 = e.to_json();
    CHECK(s2.find("\"type\":\"event\"") != std::string::npos);
    CHECK(s2.find("\"name\":\"run_finished\"") != std::string::npos);
    CHECK(s2.find("\"ms\":42") != std::string::npos);
}

// (test_preview_header removed: the core-side gid/codec preview header was
// deleted with the v9 vars/preview-core teardown; the `expose` plugin frames its
// own XEX1 output and the core no longer owns a binary preview header type.)

static void test_parse_edge_cases() {
    SECTION("parse_cmd edge cases");
    // No args field → default {}
    auto p = parse_cmd(R"({"type":"cmd","id":9,"name":"ping"})");
    CHECK(p.has_value());
    if (p) {
        CHECK(p->id == 9);
        CHECK(p->name == "ping");
        CHECK(p->args_json == "{}");
    }

    // Wrong type
    auto p2 = parse_cmd(R"({"type":"rsp","id":1})");
    CHECK(!p2.has_value());

    // Escaped string in name
    auto p3 = parse_cmd(R"({"type":"cmd","id":1,"name":"say \"hi\""})");
    CHECK(p3.has_value());
    if (p3) CHECK(p3->name == "say \"hi\"");

    // Nested args object
    auto p4 = parse_cmd(R"({"type":"cmd","id":2,"name":"x","args":{"a":{"b":1},"c":[1,2]}})");
    CHECK(p4.has_value());
    if (p4) {
        CHECK(p4->args_json.find("\"a\"") != std::string::npos);
        CHECK(p4->args_json.find("[1,2]") != std::string::npos);
    }
}

// strip_quotes must reverse every escape the writers can emit — including the
// \uXXXX / \b / \f forms used for control chars. Decoding only the
// \" \\ \n \r \t subset silently corrupted any name/path carrying one (a
// control char came back as a literal "uXXXX").
static void test_strip_quotes_unescape() {
    SECTION("strip_quotes full unescape");
    auto unq = [](std::string s) { detail::strip_quotes(s); return s; };

    // \uXXXX in the BMP (control char + a 2-byte UTF-8 code point).
    CHECK(unq("\"a\\u0007b\"") == std::string("a\x07" "b"));
    CHECK(unq("\"caf\\u00e9\"") == std::string("caf\xC3\xA9"));   // café (U+00E9)
    // \b and \f.
    CHECK(unq("\"x\\by\\fz\"") == std::string("x\by\fz"));
    // A surrogate pair → 4-byte UTF-8 (U+1F600 emoji).
    CHECK(unq("\"\\ud83d\\ude00\"") == std::string("\xF0\x9F\x98\x80"));
    // The basic escapes still work, and plain UTF-8 bytes pass through.
    CHECK(unq("\"say \\\"hi\\\"\\n\"") == std::string("say \"hi\"\n"));
    CHECK(unq("\"caf\xC3\xA9\"") == std::string("caf\xC3\xA9"));
    // Malformed \u (too few hex) degrades gracefully without crashing.
    std::string bad = "\"a\\u12\""; detail::strip_quotes(bad);
    CHECK(bad.find('a') != std::string::npos);

    // Round-trip through the matching writer: escape then strip recovers it.
    std::string esc; json_escape_into(esc, std::string("ctrl\x01 caf\xC3\xA9 \t end"));
    std::string back = esc; detail::strip_quotes(back);
    CHECK(back == std::string("ctrl\x01 caf\xC3\xA9 \t end"));
}

// Review 09 finding 2: an envelope parse_cmd() rejects should still surface a
// recoverable id so the client gets a correlated error instead of stalling.
static void test_recover_cmd_id() {
    SECTION("recover_cmd_id");
    // Wrong type but valid id → recoverable.
    auto a = recover_cmd_id(R"({"type":"rsp","id":17,"name":"x"})");
    CHECK(a.has_value()); if (a) CHECK(*a == 17);
    // Missing name but valid id → recoverable.
    auto b = recover_cmd_id(R"({"type":"cmd","id":42})");
    CHECK(b.has_value()); if (b) CHECK(*b == 42);
    // Quoted id → recoverable.
    auto c = recover_cmd_id(R"({"type":"cmd","id":"8","name":"ping"})");
    CHECK(c.has_value()); if (c) CHECK(*c == 8);
    // No id at all → nothing to correlate to.
    CHECK(!recover_cmd_id(R"({"type":"cmd","name":"ping"})").has_value());
    // Over-int64 id (a JS bigint) → uncorrelatable, fall back to log.
    CHECK(!recover_cmd_id(R"({"type":"cmd","id":99999999999999999999,"name":"x"})").has_value());
}

// The single dispatch shell (review 09 findings 1-2): a throwing handler must
// become rsp ok:false correlated to the id; a malformed envelope with a
// recoverable id gets a correlated error + a reject bump; an unparseable one logs.
static void test_dispatch_shell() {
    SECTION("dispatch_command_guarded");
    struct Sink {
        long long err_id = -1; std::string err_msg;
        std::string log_msg;
        int rejects = 0;
        bool invoked = false;
    } s;
    auto send_err = [&](int64_t id, std::string m) { s.err_id = id; s.err_msg = std::move(m); };
    auto send_log = [&](std::string m) { s.log_msg = std::move(m); };
    auto on_reject = [&] { ++s.rejects; };

    // (a) a handler that throws → rsp ok:false with the command's id + what().
    s = Sink{};
    dispatch_command_guarded(
        R"({"type":"cmd","id":31,"name":"boom"})",
        send_err, send_log, on_reject,
        [&](std::string_view, int64_t, const ParsedCmd*) -> bool {
            s.invoked = true;
            throw std::runtime_error("handler blew up");
        });
    CHECK(s.invoked);
    CHECK(s.err_id == 31);
    CHECK(s.err_msg == "handler blew up");
    CHECK(s.rejects == 0);          // a throw is not a malformed-envelope reject

    // (a') a non-std throw → generic error, still correlated.
    s = Sink{};
    dispatch_command_guarded(
        R"({"type":"cmd","id":32,"name":"boom"})",
        send_err, send_log, on_reject,
        [&](std::string_view, int64_t, const ParsedCmd*) -> bool { throw 7; });
    CHECK(s.err_id == 32);
    CHECK(s.err_msg.find("unknown error") != std::string::npos);

    // (b) malformed envelope WITH a recoverable id → correlated error + reject.
    s = Sink{};
    dispatch_command_guarded(
        R"({"type":"rsp","id":58,"name":"x"})",   // wrong type → parse_cmd rejects
        send_err, send_log, on_reject,
        [&](std::string_view, int64_t, const ParsedCmd*) -> bool { s.invoked = true; return true; });
    CHECK(!s.invoked);              // never reached a handler
    CHECK(s.err_id == 58);
    CHECK(s.err_msg == "malformed command");
    CHECK(s.rejects == 1);

    // (c) unparseable envelope with NO id → log-only + reject (nothing to correlate).
    s = Sink{};
    dispatch_command_guarded(
        R"(not even json)",
        send_err, send_log, on_reject,
        [&](std::string_view, int64_t, const ParsedCmd*) -> bool { return true; });
    CHECK(s.err_id == -1);          // no correlated rsp
    CHECK(s.log_msg.find("malformed cmd") != std::string::npos);
    CHECK(s.rejects == 1);

    // (d) unknown command → correlated unknown-command error, NOT a reject.
    s = Sink{};
    dispatch_command_guarded(
        R"({"type":"cmd","id":5,"name":"nope"})",
        send_err, send_log, on_reject,
        [&](std::string_view, int64_t, const ParsedCmd*) -> bool { return false; });
    CHECK(s.err_id == 5);
    CHECK(s.err_msg.find("unknown command") != std::string::npos);
    CHECK(s.rejects == 0);

    // (e) a well-formed command reaches its handler with the parsed id.
    s = Sink{};
    dispatch_command_guarded(
        R"({"type":"cmd","id":99,"name":"ping"})",
        send_err, send_log, on_reject,
        [&](std::string_view name, int64_t id, const ParsedCmd*) -> bool {
            s.invoked = true; CHECK(name == "ping"); CHECK(id == 99); return true;
        });
    CHECK(s.invoked);
    CHECK(s.err_id == -1);
    CHECK(s.rejects == 0);
}

int main() {
    test_cmd_encode();
    test_cmd_parse_fixture();
    test_rsp_ok();
    test_rsp_err();
    test_log_event();
    test_parse_edge_cases();
    test_strip_quotes_unescape();
    test_recover_cmd_id();
    test_dispatch_shell();

    if (g_failures == 0) {
        std::printf("\nALL PROTOCOL TESTS PASSED\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
        return 1;
    }
}
