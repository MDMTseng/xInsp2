//
// test_controls.cpp — the native half of the `controls` pluginlet
// (pluginlets/controls/controls.hpp). Verifies the contract that lets a plugin
// declare its params once and get get_def / set_def / the $schema UI tree /
// thread-safe access: schema emission (tabs/sections/collapse/widget/constraints),
// set_def validation (clamp + enum reject + absent-tolerant + readout/button
// unwritable), readout push, snapshot typing, and set_def/snapshot concurrency.
//
#include <controls/controls.hpp>
#include <xi/xi_test.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

using xi::pluginlet::Controls;
using xi::Json;

namespace {

// The doc-37 example panel, reused across tests. Controls holds a shared_mutex so
// it is non-movable — populate a reference rather than return by value.
void build(Controls& c) {
    c.tab("Capture")
        .section("Basic")
            .slider("fps", 30, 1, 60)
            .numpad("gain", 1.0, 0.1, 4.0)
        .section("Advanced").collapsed()
            .toggle("invert", false)
            .enumsel("mode", "fast", {"fast", "accurate"})
     .tab("Output")
            .readout("last", "Last measured");
}

// find first node with type==t (depth-first) in a parsed $schema tree
Json find_type(const Json& node, const char* t) {
    if (node["type"].as_string() == t) return Json::parse(node.dump());  // copy out
    Json kids = node["children"];
    for (int i = 0; ; ++i) {
        Json k = kids[i];
        if (!k.valid()) break;
        Json r = find_type(k, t);
        if (r.valid()) return r;
    }
    return Json{};
}

} // namespace

// get_def carries every declared value at its correct type + the meta fields.
XI_TEST(get_def_emits_values_and_meta) {
    Controls c; build(c);
    Json d = Json::parse(c.get_def());
    XI_EXPECT(d.valid());
    XI_EXPECT_EQ(d["fps"].as_int(-1), 30);
    XI_EXPECT(d["gain"].as_double(-1) > 0.99 && d["gain"].as_double() < 1.01);
    XI_EXPECT_EQ(d["invert"].as_bool(true), false);
    XI_EXPECT_EQ(d["mode"].as_string("?"), std::string("fast"));
    XI_EXPECT_EQ(d["$v"].as_int(-1), 1);
    XI_EXPECT_EQ(d["$rev"].as_int(-1), 1);
    XI_EXPECT(d["$schema"].valid());
}

// The $schema is a tree: tabs at the root, a collapsed section, a slider leaf with
// min/max, and an enum leaf carrying its options.
XI_TEST(schema_is_a_layout_tree) {
    Controls c; build(c);
    Json d = Json::parse(c.get_def());
    Json schema = d["$schema"];

    Json tab = find_type(schema, "tab");
    XI_EXPECT(tab.valid());
    XI_EXPECT_EQ(tab["title"].as_string("?"), std::string("Capture"));

    Json sec = find_type(schema, "section");
    XI_EXPECT(sec.valid());

    // the Advanced section is collapsed-by-default
    bool found_collapsed = false;
    std::function<void(const Json&)> walk = [&](const Json& n) {
        if (n["type"].as_string() == "section" && n["collapsed"].as_bool(false))
            found_collapsed = true;
        Json kids = n["children"];
        for (int i = 0;; ++i) { Json k = kids[i]; if (!k.valid()) break; walk(k); }
    };
    walk(schema);
    XI_EXPECT(found_collapsed);

    // a control leaf: the fps slider with declared bounds
    bool found_slider = false;
    std::function<void(const Json&)> walk2 = [&](const Json& n) {
        if (n["type"].as_string() == "control" && n["key"].as_string() == "fps") {
            found_slider = true;
            XI_EXPECT_EQ(n["widget"].as_string("?"), std::string("slider"));
            XI_EXPECT_EQ(n["min"].as_int(-1), 1);
            XI_EXPECT_EQ(n["max"].as_int(-1), 60);
        }
        Json kids = n["children"];
        for (int i = 0;; ++i) { Json k = kids[i]; if (!k.valid()) break; walk2(k); }
    };
    walk2(schema);
    XI_EXPECT(found_slider);
}

// set_def clamps numerics to the declared range (never trust the client).
XI_TEST(set_def_clamps_numeric) {
    Controls c; build(c);
    c.set_def(R"({"fps": 999, "gain": -5})");
    auto s = c.snapshot();
    XI_EXPECT_EQ(s.i("fps"), 60);            // clamped to max
    XI_EXPECT(s.f("gain") > 0.099 && s.f("gain") < 0.101);   // clamped to min 0.1

    c.set_def(R"({"fps": -10})");
    XI_EXPECT_EQ(c.snapshot().i("fps"), 1);  // clamped to min
}

// set_def rejects an unknown enum option and keeps the prior value.
XI_TEST(set_def_rejects_unknown_enum) {
    Controls c; build(c);
    c.set_def(R"({"mode": "accurate"})");
    XI_EXPECT_EQ(c.snapshot().s("mode"), std::string("accurate"));
    c.set_def(R"({"mode": "telepathy"})");           // not an option
    XI_EXPECT_EQ(c.snapshot().s("mode"), std::string("accurate"));   // unchanged
}

// Absent keys are tolerated (an old instance.json missing a param keeps defaults).
XI_TEST(set_def_absent_key_tolerated) {
    Controls c; build(c);
    c.set_def(R"({"fps": 24})");                      // only fps present
    auto s = c.snapshot();
    XI_EXPECT_EQ(s.i("fps"), 24);
    XI_EXPECT_EQ(s.s("mode"), std::string("fast"));   // untouched default
    XI_EXPECT(!Json::parse("not json").valid());
    XI_EXPECT_EQ(c.set_def("not json"), false);       // malformed → rejected
}

// A button (command, no value) and a readout (output) are NOT writable via set_def.
XI_TEST(button_and_readout_not_writable) {
    Controls c;
    c.button("reset", "Reset").readout("val", "Value");
    // buttons carry no value; a def naming the command must not create/alter one
    c.set_def(R"({"reset": 1, "val": "hacked"})");
    auto s = c.snapshot();
    XI_EXPECT_EQ(s.s("val"), std::string(""));        // readout untouched by set_def

    c.set_readout("val", 12.34);                       // plugin pushes output
    XI_EXPECT_EQ(c.snapshot().s("val"), std::string("12.34"));

    // the button appears in the schema as a command, no value in get_def
    Json d = Json::parse(c.get_def());
    XI_EXPECT(!d["reset"].valid());                    // no value key for the button
}

// Presentation leaves (title / label / divider) appear in the schema but carry no
// value; radio is enum data with a radio widget.
XI_TEST(presentation_and_radio_widgets) {
    Controls c;
    c.title("Thresholds")
     .radio("polarity", "rising", {"rising", "falling", "any"})
     .divider()
     .label("Advanced options below")
     .slider("min_strength", 20, 0, 255);

    // collect every leaf widget in the tree
    Json d = Json::parse(c.get_def());
    std::vector<std::string> widgets;
    std::string radio_options_seen;
    std::function<void(const Json&)> walk = [&](const Json& n) {
        if (n["type"].as_string() == "control") {
            widgets.push_back(n["widget"].as_string());
            if (n["key"].as_string() == "polarity") radio_options_seen = n["options"][1].as_string();
        }
        Json kids = n["children"];
        for (int i = 0;; ++i) { Json k = kids[i]; if (!k.valid()) break; walk(k); }
    };
    walk(d["$schema"]);

    auto has = [&](const char* w) {
        for (auto& x : widgets) if (x == w) return true; return false; };
    XI_EXPECT(has("title"));
    XI_EXPECT(has("radio"));
    XI_EXPECT(has("divider"));
    XI_EXPECT(has("label"));
    XI_EXPECT_EQ(radio_options_seen, std::string("falling"));   // radio carries options

    // title/label/divider have no key → no value in get_def; radio does.
    XI_EXPECT(!d["Thresholds"].valid());
    XI_EXPECT_EQ(d["polarity"].as_string("?"), std::string("rising"));

    // radio validates like enumsel
    c.set_def(R"({"polarity": "falling"})");
    XI_EXPECT_EQ(c.snapshot().s("polarity"), std::string("falling"));
    c.set_def(R"({"polarity": "sideways"})");                     // not an option
    XI_EXPECT_EQ(c.snapshot().s("polarity"), std::string("falling"));
}

// snapshot returns typed values lock-free for the caller.
XI_TEST(snapshot_types) {
    Controls c; build(c);
    c.set_def(R"({"fps": 45, "gain": 2.5, "invert": true, "mode": "accurate"})");
    auto s = c.snapshot();
    XI_EXPECT_EQ(s.i("fps"), 45);
    XI_EXPECT(s.f("gain") > 2.49 && s.f("gain") < 2.51);
    XI_EXPECT_EQ(s.b("invert"), true);
    XI_EXPECT_EQ(s.s("mode"), std::string("accurate"));
    XI_EXPECT_EQ(s.i("nonexistent"), 0);               // missing → zero, no crash
}

// Concurrency: hammering set_def from one thread while another snapshots must not
// crash or tear (shared_mutex + snapshot copy). Values stay within the clamp range.
XI_TEST(concurrent_set_and_snapshot) {
    Controls c; build(c);
    std::atomic<bool> stop{false};
    std::atomic<int>  reads{0};
    std::thread writer([&] {
        for (int i = 0; i < 5000; ++i)
            c.set_def(std::string("{\"fps\":") + std::to_string(i % 120) + "}");
        stop = true;
    });
    std::thread reader([&] {
        while (!stop) {
            int fps = c.snapshot().i("fps");
            XI_EXPECT(fps >= 1 && fps <= 60);          // always a clamped, whole value
            ++reads;
        }
    });
    writer.join(); reader.join();
    XI_EXPECT(reads.load() > 0);
}

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}
