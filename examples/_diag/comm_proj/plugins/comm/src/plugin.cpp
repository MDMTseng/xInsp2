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
        std::lock_guard<std::mutex> lk(mu_);   // send() may race across lane workers
        Stream& s = streams_[stream];
        if (op == "send") {
            yyjson_val* sj = yyjson_obj_get(root, "seq");
            uint32_t seq = (sj && yyjson_is_num(sj)) ? (uint32_t)yyjson_get_num(sj) : 0;
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
        yyjson_doc_free(doc);
        return out;
    }
private:
    static std::string jstr(yyjson_val* o, const char* k) {
        yyjson_val* v = yyjson_obj_get(o, k);
        return (v && yyjson_is_str(v) && yyjson_get_str(v)) ? yyjson_get_str(v) : "";
    }
    struct Stream { uint32_t next = 0; std::map<uint32_t, std::string> buf; std::string flushed; };
    std::mutex mu_;
    std::unordered_map<std::string, Stream> streams_;
};
XI_PLUGIN_IMPL(Comm)
