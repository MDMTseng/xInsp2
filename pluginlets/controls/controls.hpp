#pragma once
//
// controls.hpp — the NATIVE half of the `controls` pluginlet (doc 37): the DEFAULT
// plugin UI. A plugin declares its parameters once as a UI TREE (tabs / collapsible
// sections holding sliders / numpads / toggles / dropdowns / buttons / read-only
// readouts); this helper then serves get_def / set_def / the `$schema` UI tree /
// thread-safe access — replacing the get_def/set_def marshalling boilerplate every
// plugin hand-writes today (mock_camera's is the exact thing this removes).
//
// PARASITIC (doc 37): no owner identity, no ABI surface of its own — it is a plain
// member of the host, and the host DELEGATES its get_def/set_def to it:
//
//     class MyPlugin : public xi::Plugin {
//         xi::pluginlet::Controls ctl_;
//     public:
//         MyPlugin(const xi_host_api* h, const std::string& n) : xi::Plugin(h, n) {
//             ctl_.tab("Capture")
//                   .section("Basic").slider("fps",30,1,60).numpad("gain",1.0,0.1,4.0)
//                   .section("Advanced").collapsed().enumsel("mode","fast",{"fast","accurate"})
//                 .tab("Output").readout("last","Last measured");
//         }
//         std::string get_def() const override        { return ctl_.get_def(); }
//         bool set_def(const std::string& j) override  { return ctl_.set_def(j); }
//         void process(...) override { auto s = ctl_.snapshot(); int fps = s.i("fps"); … }
//     };
//
// THREE things it centralizes for EVERY plugin (doc 37):
//   1. validation declared ONCE — the descriptor's min/max/enum; set_def clamps /
//      rejects against it (never trusts the client), the UI mirrors it.
//   2. thread safety — set_def (arrives on any thread) vs process (a dispatch
//      thread) is handled by a shared_mutex + a per-frame snapshot(), once.
//   3. `$v` so an old persisted def still parses.
//
// STRUCTURE vs VALUES (doc 37): the tree + descriptors are built in the ctor and
// IMMUTABLE afterwards (so they need no lock); only the values are mutable and
// mutex-guarded. `$rev` lets the renderer cache the tree and only re-render it when
// the structure changes — here the structure never changes post-construction, so
// `$rev` is constant, but the field is emitted for a future dynamic-tree host.
//
// Widget vocabulary is this pluginlet's CONTRACT, not core ABI (doc 37): a new
// widget type is a version bump here, the frozen ABI never moves.
//
#include <xi/xi_json.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace xi::pluginlet {

class Controls {
    // Declared first so the public Snapshot (below) can name Value.
    // Static = a presentation-only leaf (title / label / divider): no value, no key.
    enum class Kind { Float, Bool, Enum, Str, Button, Readout, Static };
    struct Value { Kind kind = Kind::Float; double num = 0; bool b = false; std::string s; };
    struct Node {
        bool container = false;
        std::string type, title;          // container
        bool collapsed = false;
        std::vector<std::unique_ptr<Node>> children;
        std::string key, command, label, widget;   // leaf
        Kind kind = Kind::Float;
        double lo = 0, hi = 0;
        std::vector<std::string> options;
    };

public:
    Controls() : root_(mk_container_("root", "")) {
        cur_ = root_.get();
    }

    // ---- layout builders (containers) ------------------------------------
    Controls& tab(std::string title) {
        cur_tab_ = add_(root_.get(), mk_container_("tab", std::move(title)));
        cur_     = cur_tab_;
        last_container_ = cur_tab_;
        return *this;
    }
    Controls& section(std::string title) {
        Node* parent = cur_tab_ ? cur_tab_ : root_.get();
        cur_ = add_(parent, mk_container_("section", std::move(title)));
        last_container_ = cur_;
        return *this;
    }
    // Mark the most-recently-opened container collapsed-by-default (view state; the
    // operator expanding it is browser-local, never persisted — doc 37).
    Controls& collapsed() {
        if (last_container_) last_container_->collapsed = true;
        return *this;
    }

    // ---- control builders (leaves) ---------------------------------------
    Controls& slider(std::string key, double def, double lo, double hi) {
        return num_(std::move(key), def, lo, hi, "slider");
    }
    Controls& numpad(std::string key, double def, double lo, double hi) {
        return num_(std::move(key), def, lo, hi, "numpad");   // touch numeric entry
    }
    Controls& toggle(std::string key, bool def) {
        auto* n = add_leaf_(Kind::Bool, key, "toggle");
        values_[key] = Value{Kind::Bool, 0, def, ""};
        (void)n; return *this;
    }
    // enumsel = a DROPDOWN; radio = the SAME pick-one-of-N data as a radio group
    // (presentation differs, the contract is identical).
    Controls& enumsel(std::string key, std::string def, std::vector<std::string> options) {
        return enum_(std::move(key), std::move(def), std::move(options), "dropdown");
    }
    Controls& radio(std::string key, std::string def, std::vector<std::string> options) {
        return enum_(std::move(key), std::move(def), std::move(options), "radio");
    }
    Controls& text(std::string key, std::string def) {
        add_leaf_(Kind::Str, key, "text");
        values_[key] = Value{Kind::Str, 0, false, std::move(def)};
        return *this;
    }
    // A button fires a named command (via the host's exchange), NOT a bound value
    // (doc 37: commands are distinct from config). No value stored.
    Controls& button(std::string command, std::string label) {
        auto* n = add_leaf_(Kind::Button, /*key*/"", "button");
        n->command = std::move(command);
        n->label   = std::move(label);
        return *this;
    }
    // A read-only output the PLUGIN pushes (set_readout); shown, never written by
    // set_def. Stored as a formatted string.
    Controls& readout(std::string key, std::string label) {
        auto* n = add_leaf_(Kind::Readout, key, "readout");
        n->label = std::move(label);
        values_[key] = Value{Kind::Readout, 0, false, ""};
        return *this;
    }

    // ---- presentation-only leaves (no value, no key) ---------------------
    // A section heading, a paragraph of help/body text, a horizontal rule. They ride
    // the schema tree for the renderer but hold no config — get_def/set_def ignore
    // them (they carry no key).
    Controls& title(std::string text) {
        add_leaf_(Kind::Static, "", "title")->label = std::move(text); return *this;
    }
    Controls& label(std::string text) {
        add_leaf_(Kind::Static, "", "label")->label = std::move(text); return *this;
    }
    Controls& divider() { add_leaf_(Kind::Static, "", "divider"); return *this; }

    // ---- the delegated def surface ---------------------------------------
    // {<key>:<value>…, "$v":N, "$rev":R, "$schema":<tree>}
    std::string get_def() const {
        std::shared_lock<std::shared_mutex> lk(mu_);
        Json o = Json::object();
        for (const auto& [key, node] : leaf_by_key_) {
            if (node->kind == Kind::Button) continue;   // no value
            auto it = values_.find(key);
            if (it == values_.end()) continue;
            emit_value_(o, key, node->kind, it->second);
        }
        o.set("$v", (int)v_);
        o.set("$rev", (int)rev_);
        o.set("$schema", emit_node_(*root_));
        return o.dump();
    }

    // Apply a def: for each WRITABLE param present in `j`, validate+clamp against its
    // descriptor and store. Absent keys keep their value (tolerant — an old
    // instance.json lacking a param must default cleanly, not fail). Buttons and
    // readouts are never written here.
    bool set_def(const std::string& j) {
        Json p = Json::parse(j);
        if (!p.valid()) return false;
        std::unique_lock<std::shared_mutex> lk(mu_);
        for (const auto& [key, node] : leaf_by_key_) {
            if (node->kind == Kind::Button || node->kind == Kind::Readout) continue;
            Json v = p[key.c_str()];
            if (!v.valid()) continue;
            Value& val = values_[key];
            switch (node->kind) {
                case Kind::Float:
                    val.num = clampd_(v.as_double(val.num), node->lo, node->hi);
                    break;
                case Kind::Bool:
                    val.b = v.as_bool(val.b);
                    break;
                case Kind::Enum: {
                    std::string s = v.as_string(val.s);
                    if (in_options_(*node, s)) val.s = s;   // reject unknown option
                    break;
                }
                case Kind::Str:
                    val.s = v.as_string(val.s);
                    break;
                default: break;
            }
        }
        return true;
    }

    // ---- plugin-side output push (readouts) ------------------------------
    void set_readout(const std::string& key, const std::string& s) {
        std::unique_lock<std::shared_mutex> lk(mu_);
        auto it = values_.find(key);
        if (it != values_.end() && it->second.kind == Kind::Readout) it->second.s = s;
    }
    void set_readout(const std::string& key, double num) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.4g", num);
        set_readout(key, std::string(buf));
    }

    // ---- hot-path read ---------------------------------------------------
    struct Snapshot {
        std::unordered_map<std::string, Value> v;
        int i(const char* k) const {
            auto it = v.find(k); return it == v.end() ? 0 : (int)std::llround(it->second.num);
        }
        double f(const char* k) const {
            auto it = v.find(k); return it == v.end() ? 0.0 : it->second.num;
        }
        bool b(const char* k) const {
            auto it = v.find(k); return it == v.end() ? false : it->second.b;
        }
        std::string s(const char* k) const {
            auto it = v.find(k); return it == v.end() ? std::string() : it->second.s;
        }
    };
    // One shared_lock, copy the values out; the caller then reads lock-free for the
    // whole frame (mock_camera's "snapshot config once per frame", generalized).
    Snapshot snapshot() const {
        std::shared_lock<std::shared_mutex> lk(mu_);
        Snapshot sn; sn.v = values_; return sn;
    }

private:
    static std::unique_ptr<Node> mk_container_(std::string type, std::string title) {
        auto n = std::make_unique<Node>();
        n->container = true; n->type = std::move(type); n->title = std::move(title);
        return n;
    }
    // Append a child (unique_ptr storage keeps the pointed-to Node stable across
    // vector growth, so the cursor pointers below never dangle).
    Node* add_(Node* parent, std::unique_ptr<Node> child) {
        parent->children.push_back(std::move(child));
        return parent->children.back().get();
    }
    Node* add_leaf_(Kind kind, const std::string& key, const char* widget) {
        auto leaf = std::make_unique<Node>();
        leaf->container = false; leaf->kind = kind; leaf->key = key; leaf->widget = widget;
        Node* p = add_(cur_, std::move(leaf));
        if (!key.empty()) leaf_by_key_[key] = p;
        return p;
    }
    Controls& num_(std::string key, double def, double lo, double hi, const char* widget) {
        auto* n = add_leaf_(Kind::Float, key, widget);
        n->lo = lo; n->hi = hi;
        values_[key] = Value{Kind::Float, clampd_(def, lo, hi), false, ""};
        return *this;
    }
    Controls& enum_(std::string key, std::string def, std::vector<std::string> options,
                    const char* widget) {
        auto* n = add_leaf_(Kind::Enum, key, widget);
        n->options = std::move(options);
        values_[key] = Value{Kind::Enum, 0, false, std::move(def)};
        return *this;
    }

    static double clampd_(double v, double lo, double hi) {
        if (lo >= hi) return v;                       // no real range → no clamp
        return v < lo ? lo : (v > hi ? hi : v);
    }
    static bool in_options_(const Node& n, const std::string& s) {
        for (const auto& o : n.options) if (o == s) return true;
        return false;
    }
    static void emit_value_(Json& o, const std::string& key, Kind kind, const Value& val) {
        switch (kind) {
            case Kind::Float:                       o.set(key.c_str(), val.num); break;
            case Kind::Bool:                        o.set(key.c_str(), val.b);   break;
            case Kind::Enum: case Kind::Str:
            case Kind::Readout:                     o.set(key.c_str(), val.s);   break;
            default: break;
        }
    }
    // Recursively serialize a node into the `$schema` tree (structure only — values
    // are emitted flat above, kept separate so a value change need not re-render).
    Json emit_node_(const Node& n) const {
        Json o = Json::object();
        o.set("type", n.container ? n.type.c_str() : "control");
        if (!n.title.empty()) o.set("title", n.title.c_str());
        if (n.collapsed) o.set("collapsed", true);
        if (n.container) {
            Json arr = Json::array();
            for (const auto& c : n.children) arr.push(emit_node_(*c));
            o.set("children", arr);
        } else {
            o.set("widget", n.widget.c_str());
            if (!n.key.empty())     o.set("key", n.key.c_str());
            if (!n.command.empty()) o.set("command", n.command.c_str());
            if (!n.label.empty())   o.set("label", n.label.c_str());
            if (n.kind == Kind::Float) { o.set("min", n.lo); o.set("max", n.hi); }
            if (n.kind == Kind::Enum) {
                Json opts = Json::array();
                for (const auto& s : n.options) opts.push(s.c_str());
                o.set("options", opts);
            }
        }
        return o;
    }

    std::unique_ptr<Node> root_;
    Node* cur_ = nullptr;             // where leaves are added
    Node* cur_tab_ = nullptr;
    Node* last_container_ = nullptr;  // target of collapsed()
    std::unordered_map<std::string, Node*> leaf_by_key_;   // stable ptrs (unique_ptr)

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, Value> values_;        // guarded by mu_

    uint32_t v_   = 1;   // schema version
    uint32_t rev_ = 1;   // structure revision (constant here; static tree)
};

} // namespace xi::pluginlet
