// Comm reorder template (Phase C) — the "comms as a plugin" replacement for the
// removed xi::comms core API. Keeps N INDEPENDENT ordered streams keyed by an
// explicit string stream_id; each pool reorders by the emitter-assigned seq and
// FLUSHES in strict order. A real impl writes each flushed payload to its PLC
// socket here; this template appends to an observable per-stream log so a test
// can assert ordering. Handles the parallel-dispatch "順序問題": workers call
// send out of order, the pool restores order.
//
// Contract: seq is contiguous from 0 per stream (the emitter guarantees this —
// strict no-drop, or a blank for any seq it skips — so a pool never stalls).
//
//   exchange {"op":"send","stream":"S","seq":N,"payload":"..."}  -> buffer+flush
//   exchange {"op":"drain","stream":"S"}  -> {"flushed":"p0,p1,..."} (seq order)
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
        std::lock_guard<std::mutex> lk(mu_);   // send() may race across lane workers
        Stream& s = streams_[stream];
        if (op == "send") {
            cJSON* sj = cJSON_GetObjectItem(root, "seq");
            uint32_t seq = (sj && cJSON_IsNumber(sj)) ? (uint32_t)sj->valuedouble : 0;
            s.buf[seq] = jstr(root, "payload");
            for (auto it = s.buf.find(s.next); it != s.buf.end(); it = s.buf.find(s.next)) {
                if (!s.flushed.empty()) s.flushed += ",";
                s.flushed += it->second;       // <-- real impl: write to PLC socket
                s.buf.erase(it);
                ++s.next;
            }
        } else if (op == "drain") {
            out = "{\"flushed\":\"" + s.flushed + "\"}";
        }
        cJSON_Delete(root);
        return out;
    }
private:
    static std::string jstr(cJSON* o, const char* k) {
        cJSON* v = cJSON_GetObjectItem(o, k);
        return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
    }
    struct Stream { uint32_t next = 0; std::map<uint32_t, std::string> buf; std::string flushed; };
    std::mutex mu_;
    std::unordered_map<std::string, Stream> streams_;
};
XI_PLUGIN_IMPL(Comm)
