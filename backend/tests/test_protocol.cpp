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

static void test_preview_header() {
    SECTION("preview header encode/decode");
    PreviewHeader h;
    h.gid = 100;
    h.codec = static_cast<uint32_t>(Codec::JPEG);
    h.width = 4000;
    h.height = 5000;
    h.channels = 3;

    uint8_t buf[kPreviewHeaderSize];
    encode_preview_header(h, buf);
    PreviewHeader back = decode_preview_header(buf);
    CHECK(back.gid == h.gid);
    CHECK(back.codec == h.codec);
    CHECK(back.width == h.width);
    CHECK(back.height == h.height);
    CHECK(back.channels == h.channels);
}

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

int main() {
    test_cmd_encode();
    test_cmd_parse_fixture();
    test_rsp_ok();
    test_rsp_err();
    test_vars_encode();
    test_log_event();
    test_preview_header();
    test_parse_edge_cases();

    if (g_failures == 0) {
        std::printf("\nALL PROTOCOL TESTS PASSED\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
        return 1;
    }
}
