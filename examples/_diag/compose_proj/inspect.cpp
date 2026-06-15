// Composition wiring (乙-i): one script branches on which emitter dispatched the
// run. A raw camera frame -> feed the collector. A collector (correlated) frame
// -> inspect + send to comm ordered by FRAME number (so the PLC stream is frame-
// ordered despite the 4-thread lane completing out of order).
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <yyjson.h>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
static int jint(const std::string& s, const char* k, int def) {
    int v = def;
    if (yyjson_doc* j = yyjson_read(s.c_str(), s.size(), 0)) {
        yyjson_val* root = yyjson_doc_get_root(j);
        yyjson_val* x = yyjson_obj_get(root, k);
        if (yyjson_is_num(x)) v = (int)yyjson_get_num(x);
        yyjson_doc_free(j);
    }
    return v;
}
XI_SCRIPT_EXPORT void xi_inspect_entry(int) {
    auto t = xi::current_trigger();
    std::string src = t.primary_source();
    std::string id  = t.id_string();
    if (src == "collector") {
        auto r = xi::use("collector").fetch(id);
        int frame = jint(r.data(), "frame", -1);
        std::this_thread::sleep_for(std::chrono::milliseconds(std::rand() % 30));
        std::string cmd = "{\"op\":\"send\",\"stream\":\"S\",\"seq\":" + std::to_string(frame)
                        + ",\"payload\":\"f" + std::to_string(frame) + "\"}";
        xi::use("comm").exchange(cmd);
        VAR(inspected, frame);
    } else {
        auto r = xi::use(src).fetch(id);
        int frame = jint(r.data(), "frame", -1);
        std::string cmd = "{\"op\":\"accept\",\"cam\":\"" + src + "\",\"res_id\":\"" + id
                        + "\",\"frame\":" + std::to_string(frame) + "}";
        xi::use("collector").exchange(cmd);
        VAR(fed, frame);
    }
}
