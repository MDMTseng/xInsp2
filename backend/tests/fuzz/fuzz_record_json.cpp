// fuzz_record_json.cpp — coverage-guided fuzz target for the Record wire decode.
//
// Boundary under test: xi::Record::from_json_bytes(data, len) in xi_record.hpp.
// This is the JSON branch of record_from_c() (xi_abi.hpp) — i.e. the exact path
// the ABI `process()` seam takes to turn an inbound plugin/host record's bytes
// into a live xi::Record when there is no in-process yyjson doc to borrow. It
// parses with yyjson (immutable), deep-copies into a mutable doc, then the host
// reads fields back out. Fuzzing it covers: the parse, the immutable->mutable
// val copy, the DocBox refcount plumbing, and the typed getters.
//
// NOTE: xi_record.hpp transitively includes <opencv2/core.hpp> (via xi_image.hpp),
// so this target links OpenCV. The parse path itself touches no image data.
//
// Headers included READ-ONLY. A crash here in from_json_bytes / the mut-copy /
// the getters is a genuine core finding — save the reproducer and REPORT, do not
// patch core.
//
#include <cstddef>
#include <cstdint>
#include <string>

#include "xi/xi_record.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    xi::Record r = xi::Record::from_json_bytes(data, size);

    // Drive the read-back getters the host/ABI uses after decode. These pull
    // values out of the freshly materialised mutable doc.
    if (!r.empty()) {
        volatile bool b = r.get_bool("ok", false);
        (void)b;
        volatile double d = r.get_double("value", 0.0);
        (void)d;
        volatile long long i = r.get_int("id", 0);
        (void)i;
        std::string s = r.get_string("name", "");
        volatile size_t n = s.size();
        (void)n;
        // Serialize back out — the Record::data_json() writer path.
        std::string j = r.data_json();
        volatile size_t jn = j.size();
        (void)jn;
        // Nested access exercises the recursive Record wrap.
        xi::Record sub = r.get_record("meta");
        volatile bool e = sub.empty();
        (void)e;
    }
    return 0;
}
