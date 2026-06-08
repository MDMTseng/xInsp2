// Parallel-comm reorder: each of the 4 lane workers pulls its frame by id, reads
// the emitter-assigned seq from the dataInfo, sleeps a RANDOM time (so completion
// order scrambles), then sends to the comm plugin tagged with that seq. comm
// reorders by seq -> the PLC stream comes out in order despite the scramble.
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <cJSON.h>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
XI_SCRIPT_EXPORT void xi_inspect_entry(int) {
    auto t = xi::current_trigger();
    auto r = xi::use("src").fetch(t.id_string());
    int seq = -1;
    if (cJSON* j = cJSON_Parse(r.data().c_str())) {
        cJSON* s = cJSON_GetObjectItem(j, "seq");
        if (cJSON_IsNumber(s)) seq = (int)s->valuedouble;
        cJSON_Delete(j);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::rand() % 30));
    std::string cmd = "{\"op\":\"send\",\"stream\":\"S\",\"seq\":" + std::to_string(seq)
                    + ",\"payload\":\"p" + std::to_string(seq) + "\"}";
    xi::use("comm").exchange(cmd);
    VAR(frameseq, seq);
}
