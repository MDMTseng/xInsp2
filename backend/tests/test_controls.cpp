//
// test_controls.cpp — the native half of the `controls` pluginlet
// (toolbox/pluginlets/controls/controls.hpp). Verifies the contract that lets a plugin
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

// Every control can carry a caption (emitted as `label`) for grid layout.
XI_TEST(caption_on_controls) {
    Controls c;
    c.slider("fps", 30, 1, 60).caption("Frames / s")
     .toggle("invert", false).caption("Invert")
     .button("run", "Run");                    // button's own label

    Json d = Json::parse(c.get_def());
    std::string cap_fps, cap_invert, cap_run;
    std::function<void(const Json&)> walk = [&](const Json& n) {
        auto k = n["key"].as_string();
        if (k == "fps")    cap_fps    = n["label"].as_string();
        if (k == "invert") cap_invert = n["label"].as_string();
        if (n["command"].as_string() == "run") cap_run = n["label"].as_string();
        Json kids = n["children"];
        for (int i = 0;; ++i) { Json ch = kids[i]; if (!ch.valid()) break; walk(ch); }
    };
    walk(d["$schema"]);
    XI_EXPECT_EQ(cap_fps, std::string("Frames / s"));
    XI_EXPECT_EQ(cap_invert, std::string("Invert"));
    XI_EXPECT_EQ(cap_run, std::string("Run"));
    // caption is presentation only — it does not become a value key
    XI_EXPECT_EQ(d["fps"].as_int(-1), 30);
}

// Grid: a grid container carries its column count; children carry their span/rows.
XI_TEST(grid_layout) {
    Controls c;
    c.section("Params")
        .grid(12)
            .slider("x", 0, 0, 100).span(6)
            .slider("y", 0, 0, 100).span(6)      // fills the row → z wraps below
            .numpad("z", 0, 0, 100).span(4)
            .readout("mon", "Monitor").span(8).rows(2);

    Json d = Json::parse(c.get_def());
    // locate the grid and read a couple of child spans
    int columns = 0, span_x = -1, rows_mon = -1;
    std::function<void(const Json&)> walk = [&](const Json& n) {
        if (n["type"].as_string() == "grid") columns = n["columns"].as_int(0);
        if (n["key"].as_string() == "x")   span_x = n["span"].as_int(-1);
        if (n["key"].as_string() == "mon") rows_mon = n["rows"].as_int(-1);
        Json kids = n["children"];
        for (int i = 0;; ++i) { Json k = kids[i]; if (!k.valid()) break; walk(k); }
    };
    walk(d["$schema"]);
    XI_EXPECT_EQ(columns, 12);
    XI_EXPECT_EQ(span_x, 6);
    XI_EXPECT_EQ(rows_mon, 2);

    // values under the grid still bind + validate normally
    c.set_def(R"({"x": 250, "z": 40})");
    auto s = c.snapshot();
    XI_EXPECT_EQ(s.i("x"), 100);   // clamped
    XI_EXPECT_EQ(s.i("z"), 40);
}

// A live-image `view` slot: sized in the grid, references a live-view channel,
// carries no value (the frames arrive out-of-band via that channel).
XI_TEST(live_view_slot_in_grid) {
    Controls c;
    c.grid(12)
        .view("ui/cam0/preview").caption("Live").span(8).rows(6)
        .slider("gain", 1.0, 0.1, 4.0).caption("Gain").span(4);

    Json d = Json::parse(c.get_def());
    std::string ch, cap; int sp = -1, rw = -1;
    std::function<void(const Json&)> walk = [&](const Json& n) {
        if (n["widget"].as_string() == "view") {
            ch  = n["channel"].as_string();
            cap = n["label"].as_string();
            sp  = n["span"].as_int(-1);
            rw  = n["rows"].as_int(-1);
        }
        Json kids = n["children"];
        for (int i = 0;; ++i) { Json k = kids[i]; if (!k.valid()) break; walk(k); }
    };
    walk(d["$schema"]);
    XI_EXPECT_EQ(ch, std::string("ui/cam0/preview"));
    XI_EXPECT_EQ(cap, std::string("Live"));
    XI_EXPECT_EQ(sp, 8);
    XI_EXPECT_EQ(rw, 6);
    // the view carries no value key + set_def can't touch it
    XI_EXPECT(!d["ui/cam0/preview"].valid());
    c.set_def(R"({"ui/cam0/preview": "x", "gain": 2.0})");
    XI_EXPECT(c.snapshot().f("gain") > 1.99 && c.snapshot().f("gain") < 2.01);
}

// stepper / file / color widgets + a semantic-type hint on a control.
XI_TEST(extended_widgets_and_sem) {
    Controls c;
    c.stepper("count", 5, 0, 100, 1).caption("Count").sem("count")
     .file("model", "net.onnx").caption("Model")
     .color("tint", "#ff8800").caption("Tint")
     .slider("thr", 128, 0, 255).caption("Threshold").sem("threshold");

    Json d = Json::parse(c.get_def());
    XI_EXPECT_EQ(d["count"].as_int(-1), 5);
    XI_EXPECT_EQ(d["model"].as_string("?"), std::string("net.onnx"));
    XI_EXPECT_EQ(d["tint"].as_string("?"), std::string("#ff8800"));

    std::string w_count, sem_count, sem_thr; double step_count = -1;
    std::function<void(const Json&)> walk = [&](const Json& n) {
        auto k = n["key"].as_string();
        if (k == "count") { w_count = n["widget"].as_string(); sem_count = n["sem"].as_string(); step_count = n["step"].as_double(-1); }
        if (k == "thr")   { sem_thr = n["sem"].as_string(); }
        Json kids = n["children"]; for (int i = 0;; ++i) { Json ch = kids[i]; if (!ch.valid()) break; walk(ch); }
    };
    walk(d["$schema"]);
    XI_EXPECT_EQ(w_count, std::string("stepper"));
    XI_EXPECT(step_count > 0.99 && step_count < 1.01);       // stepper carries its step
    XI_EXPECT_EQ(sem_count, std::string("count"));           // semantic hint emitted
    XI_EXPECT_EQ(sem_thr, std::string("threshold"));

    // stepper clamps like any numeric
    c.set_def(R"({"count": 999})");
    XI_EXPECT_EQ(c.snapshot().i("count"), 100);
}

// range: one widget bound to two def keys; both clamp to the shared [lo,hi].
XI_TEST(range_two_keys_clamp) {
    Controls c;
    c.range("low", "high", 40, 200, 0, 255).caption("Intensity band").sem("threshold");

    Json d = Json::parse(c.get_def());
    XI_EXPECT_EQ(d["low"].as_int(-1), 40);
    XI_EXPECT_EQ(d["high"].as_int(-1), 200);

    bool found = false; std::string k2;
    std::function<void(const Json&)> walk = [&](const Json& n) {
        if (n["widget"].as_string() == "range") { found = true; k2 = n["key2"].as_string();
            XI_EXPECT_EQ(n["key"].as_string(), std::string("low"));
            XI_EXPECT_EQ(n["min"].as_int(-1), 0); XI_EXPECT_EQ(n["max"].as_int(-1), 255); }
        Json kids = n["children"]; for (int i = 0;; ++i) { Json ch = kids[i]; if (!ch.valid()) break; walk(ch); }
    };
    walk(d["$schema"]);
    XI_EXPECT(found);
    XI_EXPECT_EQ(k2, std::string("high"));                   // one widget, two keys

    // both keys clamp to [0,255]
    c.set_def(R"({"low": -20, "high": 900})");
    auto s = c.snapshot();
    XI_EXPECT_EQ(s.i("low"), 0);
    XI_EXPECT_EQ(s.i("high"), 255);
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

// comp()/channel()/as(): the native side of the UI widget registry. Same custom
// component, several instances, each bound to its OWN data source (key or channel);
// as() re-skins a typed input without touching its value contract.
XI_TEST(custom_components) {
    Controls c;
    c.grid(12)
        .comp("chart", "temp_trend").caption("Temp").span(6)
        .comp("chart", "defect_rate").caption("Defects").span(6)
        .comp("map").channel("ui/cd/points").span(12)
        .slider("pressure", 2, 0, 10).as("gauge").sem("pressure").span(6);

    // schema: three chart/map leaves carry their widget name + their own source
    int charts = 0; bool map_ch = false, gauge = false;
    Json d = Json::parse(c.get_def());
    std::function<void(const Json&)> walk = [&](const Json& n) {
        const std::string w = n["widget"].as_string();
        if (w == "chart") { ++charts;
            XI_EXPECT(n["key"].as_string() == "temp_trend" || n["key"].as_string() == "defect_rate"); }
        if (w == "map")   { map_ch = n["channel"].as_string() == "ui/cd/points";
            XI_EXPECT(!n["key"].valid()); }              // keyless comp: no def slot
        if (w == "gauge") { gauge = true;
            XI_EXPECT_EQ(n["key"].as_string(), std::string("pressure"));
            XI_EXPECT_EQ(n["max"].as_int(-1), 10); }     // as() keeps the Float descriptor
        Json kids = n["children"]; for (int i = 0;; ++i) { Json ch = kids[i]; if (!ch.valid()) break; walk(ch); }
    };
    walk(d["$schema"]);
    XI_EXPECT_EQ(charts, 2);
    XI_EXPECT(map_ch);
    XI_EXPECT(gauge);

    // keyed comp = plugin-pushed readout semantics: set_readout feeds it,
    // set_def can NEVER write it (it is output, not config)…
    c.set_readout("temp_trend", R"([1,2,3])");
    c.set_def(R"({"temp_trend": "hacked", "pressure": 99})");
    d = Json::parse(c.get_def());
    XI_EXPECT_EQ(d["temp_trend"].as_string(), std::string("[1,2,3]"));
    // …while the as()-skinned slider still validates as a Float (clamped to hi)
    XI_EXPECT_EQ(d["pressure"].as_int(-1), 10);
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
