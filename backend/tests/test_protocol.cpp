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

static void test_vars_encode() {
    SECTION("Vars::to_json");
    Vars v;
    v.run_id = 17;
    v.items.push_back({"gray",  VarKindWire::Image,   "",      "",    false, 100, false});
    v.items.push_back({"count", VarKindWire::Number,  "42",    "",    false, 0,   false});
    VarItem s;
    s.name = "label";
    s.kind = VarKindWire::String;
    s.value_str = "ok";
    v.items.push_back(s);
    VarItem b;
    b.name = "flag";
    b.kind = VarKindWire::Boolean;
    b.value_bool = true;
    v.items.push_back(b);

    std::string out = v.to_json();
    CHECK(out.find("\"type\":\"vars\"")       != std::string::npos);
    CHECK(out.find("\"run_id\":17")           != std::string::npos);
    CHECK(out.find("\"gray\"")                != std::string::npos);
    CHECK(out.find("\"gid\":100")             != std::string::npos);
    CHECK(out.find("\"value\":42")            != std::string::npos);
    CHECK(out.find("\"value\":\"ok\"")        != std::string::npos);
    CHECK(out.find("\"value\":true")          != std::string::npos);
}

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

int main() {
    test_cmd_encode();
    test_cmd_parse_fixture();
    test_rsp_ok();
    test_rsp_err();
    test_vars_encode();
    test_log_event();
    test_parse_edge_cases();
    test_strip_quotes_unescape();

    if (g_failures == 0) {
        std::printf("\nALL PROTOCOL TESTS PASSED\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
        return 1;
    }
}
