//
// schema_fixture_plugin.cpp — minimal C-ABI fixtures for the OQ-7 static Record
// field contract test (test_record_schema.cpp). One source, four roles selected
// by the SCHEMA_ROLE compile define, so a single file backs the producer + the
// three consumer variants (matched / wrong-type / missing-field).
//
// Each exports the OPTIONAL xi_plugin_record_schema (resolved by the real
// CAbiInstanceAdapter at load, exactly like prepare/commit), plus the minimal
// create/destroy so the test can drive the genuine adapter load path. Raw C
// exports (no XI_PLUGIN_IMPL) so GetProcAddress resolves them as the loader does.
//
#include <cstdio>
#include <cstring>

#include <xi/xi_abi.h>
#include <xi/xi_record.hpp>   // xi::yyjson_layout_stamp()

// Role selection. 0 = producer, 1 = matched consumer, 2 = wrong-type consumer,
// 3 = missing-field consumer.
#ifndef SCHEMA_ROLE
#define SCHEMA_ROLE 0
#endif

#if SCHEMA_ROLE == 0
  // Produces the fields the consumers below expect.
  #define SCHEMA_JSON \
    "{\"produces\":[{\"key\":\"score\",\"type\":\"double\"}," \
                   "{\"key\":\"count\",\"type\":\"int\"}," \
                   "{\"key\":\"binary\",\"type\":\"image\"}]}"
#elif SCHEMA_ROLE == 1
  // Matched consumer: score(double), count(int) — both produced upstream, and
  // count(int) consumed as int; score consumed as double. Compatible.
  #define SCHEMA_JSON \
    "{\"consumes\":[{\"key\":\"score\",\"type\":\"double\"}," \
                   "{\"key\":\"count\",\"type\":\"int\"}]}"
#elif SCHEMA_ROLE == 2
  // Wrong-type consumer: reads 'score' as a string, but upstream produces double.
  #define SCHEMA_JSON \
    "{\"consumes\":[{\"key\":\"score\",\"type\":\"string\"}]}"
#elif SCHEMA_ROLE == 3
  // Missing-field consumer: reads 'width', which no upstream stage produces.
  #define SCHEMA_JSON \
    "{\"consumes\":[{\"key\":\"width\",\"type\":\"int\"}]}"
#else
  #error "unknown SCHEMA_ROLE"
#endif

namespace { struct Inst { int dummy = 0; }; }

extern "C" {

__declspec(dllexport) int xi_plugin_abi_version(void) { return XI_ABI_MIN_COMPAT; }

__declspec(dllexport) uint32_t xi_yyjson_abi(void) { return xi::yyjson_layout_stamp(); }

__declspec(dllexport) void* xi_plugin_create(const xi_host_api*, const char*) {
    return new Inst();
}

__declspec(dllexport) void xi_plugin_destroy(void* p) { delete static_cast<Inst*>(p); }

__declspec(dllexport) void xi_plugin_process(void*, const xi_record*, xi_record_out*) {}

// The OPT-IN static contract. Same buffer convention as get_def: return bytes
// written, or the negated required size if the buffer is too small.
__declspec(dllexport) int xi_plugin_record_schema(char* buf, int cap) {
    const char* j = SCHEMA_JSON;
    int need = (int)std::strlen(j);
    if (!buf || cap < need) return -need;
    std::memcpy(buf, j, (size_t)need);
    return need;
}

} // extern "C"
