// Comm reorder (parallel-test variant of the C-1 template) — additionally logs
// the ARRIVAL order so the test can show "scrambled in, ordered out". Keeps N
// ordered streams keyed by stream_id; reorders by seq; flushes in order. send()
// is mutex-guarded because the 4 lane workers call it concurrently.
#include <xi/xi.hpp>
#include <yyjson.h>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
class Comm : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    std::string exchange(const std::string& cmd) override {
        yyjson_doc* doc = yyjson_read(cmd.c_str(), cmd.size(), 0);
        yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (!root) { yyjson_doc_free(doc); return "{}"; }
        const std::string op     = jstr(root, "op");
        const std::string stream = jstr(root, "stream");
        std::string out = "{}";
        std::lock_guard<std::mutex> lk(mu_);
        Stream& s = streams_[stream];
        if (op == "send") {
            yyjson_val* sj = yyjson_obj_get(root, "seq");
            uint32_t seq = (sj && yyjson_is_num(sj)) ? (uint32_t)yyjson_get_num(sj) : 0;
            if (!s.arrivals.empty()) s.arrivals += ",";
            s.arrivals += std::to_string(seq);             // order received (scrambled)
            s.buf[seq] = jstr(root, "payload");
            for (auto it = s.buf.find(s.next); it != s.buf.end(); it = s.buf.find(s.next)) {
                if (!s.flushed.empty()) s.flushed += ",";
                s.flushed += it->second;                   // order sent on (ordered)
                s.buf.erase(it);
                ++s.next;
            }
        } else if (op == "drain") {
            out = "{\"flushed\":\"" + s.flushed + "\",\"arrivals\":\"" + s.arrivals + "\"}";
        }
        yyjson_doc_free(doc);
        return out;
    }
private:
    static std::string jstr(yyjson_val* o, const char* k) {
        yyjson_val* v = yyjson_obj_get(o, k);
        return (v && yyjson_is_str(v) && yyjson_get_str(v)) ? yyjson_get_str(v) : "";
    }
    struct Stream { uint32_t next = 0; std::map<uint32_t, std::string> buf;
                    std::string flushed; std::string arrivals; };
    std::mutex mu_;
    std::unordered_map<std::string, Stream> streams_;
};
XI_PLUGIN_IMPL(Comm)
