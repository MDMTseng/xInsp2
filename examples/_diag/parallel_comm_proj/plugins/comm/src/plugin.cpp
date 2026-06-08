// Comm reorder (parallel-test variant of the C-1 template) — additionally logs
// the ARRIVAL order so the test can show "scrambled in, ordered out". Keeps N
// ordered streams keyed by stream_id; reorders by seq; flushes in order. send()
// is mutex-guarded because the 4 lane workers call it concurrently.
#include <xi/xi.hpp>
#include <cJSON.h>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
class Comm : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    std::string exchange(const std::string& cmd) override {
        cJSON* root = cJSON_Parse(cmd.c_str());
        if (!root) return "{}";
        const std::string op     = jstr(root, "op");
        const std::string stream = jstr(root, "stream");
        std::string out = "{}";
        std::lock_guard<std::mutex> lk(mu_);
        Stream& s = streams_[stream];
        if (op == "send") {
            cJSON* sj = cJSON_GetObjectItem(root, "seq");
            uint32_t seq = (sj && cJSON_IsNumber(sj)) ? (uint32_t)sj->valuedouble : 0;
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
        cJSON_Delete(root);
        return out;
    }
private:
    static std::string jstr(cJSON* o, const char* k) {
        cJSON* v = cJSON_GetObjectItem(o, k);
        return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
    }
    struct Stream { uint32_t next = 0; std::map<uint32_t, std::string> buf;
                    std::string flushed; std::string arrivals; };
    std::mutex mu_;
    std::unordered_map<std::string, Stream> streams_;
};
XI_PLUGIN_IMPL(Comm)
