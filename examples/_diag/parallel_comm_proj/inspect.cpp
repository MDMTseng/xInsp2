// Parallel-comm reorder: each of the 4 lane workers pulls its frame by id, reads
// the emitter-assigned seq from the dataInfo, sleeps a RANDOM time (so completion
// order scrambles), then sends to the comm plugin tagged with that seq. comm
// reorders by seq -> the PLC stream comes out in order despite the scramble.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <yyjson.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
XI_SCRIPT_EXPORT void xi_inspect_entry(int) {
    auto t = xi::current_trigger();
    auto r = xi::use("src").fetch(t.id_string());
    int seq = -1;
    if (yyjson_doc* j = yyjson_read(r.data().c_str(), r.data().size(), 0)) {
        yyjson_val* root = yyjson_doc_get_root(j);
        yyjson_val* s = yyjson_obj_get(root, "seq");
        if (yyjson_is_num(s)) seq = (int)yyjson_get_num(s);
        yyjson_doc_free(j);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::rand() % 30));
    std::string cmd = "{\"op\":\"send\",\"stream\":\"S\",\"seq\":" + std::to_string(seq)
                    + ",\"payload\":\"p" + std::to_string(seq) + "\"}";
    xi::use("comm").exchange(cmd);
    VAR(frameseq, seq);
}
