// preview_sink.cpp — multi-group "view a script's output via a plugin", post-VAR.
//
// VAR + the core vars/preview wire were removed (branch refactor/remove-var-core).
// A script surfaces what it wants to view by pushing a Record into this sink,
// tagged with a preview-group id (pg_id) — per stage, per thread, per camera, ...
//
//   #include "preview_api.hpp"          // ships with this plugin
//   xi::preview::Sink pv;               // talks to instance "preview"
//   pv.process("bright", xi::Record().set("score", s).image("img", im));
//   pv.process("dark",   xi::Record().set("score", t).image("inv", im2));
//
// Each pg_id keeps its OWN latest record. A UI tabs between groups:
//   exchange_instance({"command":"list_groups"})   -> { groups: { pg: {seen,image_count}, ... } }
//   exchange_instance({"command":"get","pg":"bright"}) -> { found, seen, data, image_count }
//
// The pg_id rides in the record under the reserved key "$pg" (set by the helper).
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <map>
#include <mutex>
#include <string>

class PreviewSink : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    static constexpr const char* kPgKey = "$pg";

    // Capture the latest surfaced record into its preview group. Images are only
    // valid for THIS call; we record how many there were (a real preview plugin
    // would JPEG-encode + ship them through its own transport).
    xi::Record process(const xi::Record& in) override {
        const std::string data = in.data_json();
        std::string pg = xi::Json::parse(data)[kPgKey].as_string("default");
        int img_n = 0;
        for (auto& [k, img] : in.images()) { (void)k; if (!img.empty()) ++img_n; }

        std::lock_guard<std::mutex> lk(mu_);
        Group& g = groups_[pg];
        g.data = data;
        g.imgs = img_n;
        ++g.seen;
        return xi::Record().set("pg", pg).set("seen", (int64_t)g.seen);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        const std::string c = p["command"].as_string();
        std::lock_guard<std::mutex> lk(mu_);

        if (c == "list_groups") {
            // { groups: { "<pg>": { seen, image_count }, ... } } — webui builds tabs from the keys.
            auto groups = xi::Json::object();
            for (auto& [pg, g] : groups_)
                groups.set(pg.c_str(), xi::Json::object()
                    .set("seen", (int64_t)g.seen)
                    .set("image_count", g.imgs));
            return xi::Json::object().set("count", (int)groups_.size())
                                     .set("groups", groups).dump();
        }
        if (c == "get" || c == "get_latest") {
            const std::string pg = p["pg"].as_string("default");
            auto it = groups_.find(pg);
            if (it == groups_.end())
                return xi::Json::object().set("found", false).set("pg", pg).dump();
            return group_json_(pg, it->second);
        }
        if (c == "clear") groups_.clear();
        return xi::Json::object().set("count", (int)groups_.size()).dump();
    }

    std::string get_def() const override {
        std::lock_guard<std::mutex> lk(mu_);
        auto names = xi::Json::array();
        for (auto& [pg, g] : groups_) { (void)g; names.push(pg); }
        return xi::Json::object().set("count", (int)groups_.size())
                                 .set("groups", names).dump();
    }
    bool set_def(const std::string&) override { return true; }

private:
    struct Group { std::string data = "{}"; int imgs = 0; long long seen = 0; };

    static std::string group_json_(const std::string& pg, const Group& g) {
        return xi::Json::object()
            .set("found", true)
            .set("pg", pg)
            .set("seen", (int64_t)g.seen)
            .set("data", g.data.empty() ? std::string("{}") : g.data)
            .set("image_count", g.imgs).dump();
    }

    mutable std::mutex            mu_;
    std::map<std::string, Group>  groups_;
};

XI_PLUGIN_IMPL(PreviewSink)
