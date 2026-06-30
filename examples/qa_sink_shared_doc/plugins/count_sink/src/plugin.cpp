//
// count_sink.cpp — ordered-sink PoC probe for the $seq shared-doc double-stamp bug.
//
// The host stamps the reserved key "$seq" onto every record it flushes to a sink.
// If a script stages the SAME Record to two sink instances, both staged items share
// ONE underlying doc; a flush that mutates it in place (add-not-put, no COW) would
// stamp "$seq" twice into the doc the SECOND sink receives. This probe counts the
// "$seq" keys in its input and reports the max it ever saw: 1 = correct, 2 = bug.
//
#include <xi/xi.hpp>
#include <xi/xi_json.hpp>
#include <cstring>

class CountSink : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        int seq_keys = 0;
        yyjson_mut_val* root = input.json();
        if (root && yyjson_mut_is_obj(root)) {
            yyjson_mut_obj_iter it;
            yyjson_mut_obj_iter_init(root, &it);
            yyjson_mut_val* k;
            while ((k = yyjson_mut_obj_iter_next(&it))) {
                const char* ks = yyjson_mut_get_str(k);
                if (ks && std::strcmp(ks, "$seq") == 0) ++seq_keys;
            }
        }
        ++calls_;
        if (seq_keys > max_seq_keys_) max_seq_keys_ = seq_keys;
        return xi::Record();   // fire-and-forget
    }

    std::string exchange(const std::string& cmd) override {
        if (cmd.find("reset") != std::string::npos) {
            max_seq_keys_ = 0; calls_ = 0;
            return "{\"ok\":true}";
        }
        return xi::Json::object()
            .set("max_seq_keys", max_seq_keys_)
            .set("calls",        calls_)
            .dump();
    }

    std::string get_def() const override { return "{}"; }
    bool        set_def(const std::string&) override { return true; }

private:
    int max_seq_keys_ = 0;
    int calls_        = 0;
};

XI_PLUGIN_IMPL(CountSink)
