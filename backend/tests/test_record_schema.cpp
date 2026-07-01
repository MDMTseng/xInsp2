//
// test_record_schema.cpp — OQ-7: the opt-in STATIC cross-plugin Record field
// contract (xi_record_schema.hpp).
//
// Proves, through the REAL adapter load path (LoadLibrary → resolve the optional
// xi_plugin_record_schema export → CAbiInstanceAdapter captures + parses it),
// that a wired producer→consumer pipeline is validated at wire time:
//   * matched contract           → PASSES (nullopt)
//   * wrong-type consumed field  → CAUGHT (type mismatch named)
//   * missing consumed field     → CAUGHT (upstream gap named)
//   * undeclared (opt-out) plugin→ imposes no constraints (current behaviour)
// Plus direct unit checks of the pure validator + parser.
//
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_record_schema.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef PRODUCER_DLL
#define PRODUCER_DLL "schema_producer.dll"
#endif
#ifndef CONSUMER_OK_DLL
#define CONSUMER_OK_DLL "schema_consumer_ok.dll"
#endif
#ifndef CONSUMER_BADTYPE_DLL
#define CONSUMER_BADTYPE_DLL "schema_consumer_badtype.dll"
#endif
#ifndef CONSUMER_MISSING_DLL
#define CONSUMER_MISSING_DLL "schema_consumer_missing.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Load a fixture DLL through the genuine adapter and return its captured schema.
// Keeps the module + instance alive for the duration via the out-params so the
// schema (owned by the adapter) stays valid; caller frees.
struct LoadedFixture {
    HMODULE dll = nullptr;
    void*   inst = nullptr;
    xi::CAbiInstanceAdapter* adapter = nullptr;
    ~LoadedFixture() { delete adapter; if (dll) FreeLibrary(dll); }
};

static bool load_fixture(const char* path, const char* name,
                         const xi_host_api* host, LoadedFixture& out) {
    out.dll = LoadLibraryA(path);
    if (!out.dll) {
        std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", path, GetLastError());
        return false;
    }
    auto create = reinterpret_cast<void* (*)(const xi_host_api*, const char*)>(
        GetProcAddress(out.dll, "xi_plugin_create"));
    if (!create) { std::fprintf(stderr, "FAIL: no create in %s\n", path); return false; }
    out.inst = create(host, name);
    // CAbiInstanceAdapter ctor resolves + parses xi_plugin_record_schema.
    out.adapter = new xi::CAbiInstanceAdapter(name, name, out.dll, out.inst,
                                              /*reentrant=*/false, /*max_concurrency=*/0);
    return true;
}

int main() {
    std::printf("[test] OQ-7 static Record field contract — wire-time validation\n");

    // ---- (A) Pure unit: parser + validator, no DLLs -------------------------
    {
        using namespace xi;
        RecordSchema prod = parse_record_schema_json(
            R"({"produces":[{"key":"score","type":"double"},{"key":"n","type":"int"}]})", 0
            + std::strlen(R"({"produces":[{"key":"score","type":"double"},{"key":"n","type":"int"}]})"));
        CHECK(prod.declared);
        CHECK(prod.produces.size() == 2);
        CHECK(prod.produces[0].key == "score" && prod.produces[0].type == FieldType::Double);

        // int↔double compatible (numeric widening); string vs int not.
        CHECK(types_compatible(FieldType::Int, FieldType::Double));
        CHECK(types_compatible(FieldType::Double, FieldType::Int));
        CHECK(!types_compatible(FieldType::String, FieldType::Int));
        CHECK(types_compatible(FieldType::Any, FieldType::Image));   // wildcard

        RecordSchema cons_ok; cons_ok.declared = true;
        cons_ok.consumes = {{"score", FieldType::Double}, {"n", FieldType::Int}};
        CHECK(!validate_record_pipeline({{"p", prod}, {"c", cons_ok}}).has_value());

        RecordSchema cons_bad; cons_bad.declared = true;
        cons_bad.consumes = {{"score", FieldType::String}};
        auto e1 = validate_record_pipeline({{"p", prod}, {"c", cons_bad}});
        CHECK(e1.has_value());
        std::printf("  unit type-mismatch: %s\n", e1 ? e1->c_str() : "(none)");

        RecordSchema cons_missing; cons_missing.declared = true;
        cons_missing.consumes = {{"width", FieldType::Int}};
        auto e2 = validate_record_pipeline({{"p", prod}, {"c", cons_missing}});
        CHECK(e2.has_value());
        std::printf("  unit missing-field: %s\n", e2 ? e2->c_str() : "(none)");

        // Opt-in: an UNDECLARED upstream stage suppresses "missing" (unknown could
        // produce it) — undeclared plugins keep current behaviour.
        RecordSchema undecl;   // declared == false
        CHECK(!validate_record_pipeline({{"legacy", undecl}, {"c", cons_missing}}).has_value());
        // But a DECLARED type clash is still caught even with an undeclared sibling.
        CHECK(validate_record_pipeline({{"legacy", undecl}, {"p", prod}, {"c", cons_bad}}).has_value());
    }

    // ---- (B) Integration: real adapter load path ----------------------------
    static xi_host_api host = xi::ImagePool::make_host_api();

    LoadedFixture producer, cons_ok, cons_bad, cons_missing;
    bool loaded =
        load_fixture(PRODUCER_DLL,         "producer0",      &host, producer)   &&
        load_fixture(CONSUMER_OK_DLL,      "consumer_ok0",   &host, cons_ok)     &&
        load_fixture(CONSUMER_BADTYPE_DLL, "consumer_bad0",  &host, cons_bad)    &&
        load_fixture(CONSUMER_MISSING_DLL, "consumer_miss0", &host, cons_missing);
    CHECK(loaded);
    if (!loaded) { std::fprintf(stderr, "\n%d FAILURES\n", g_failures + 1); return 1; }

    // Schema was captured at load, from the optional export.
    CHECK(producer.adapter->record_schema().declared);
    CHECK(producer.adapter->record_schema().produces.size() == 3);
    CHECK(cons_ok.adapter->record_schema().declared);
    CHECK(cons_ok.adapter->record_schema().consumes.size() == 2);

    auto stage = [](const LoadedFixture& f) {
        return xi::SchemaStage{ f.adapter->name(), f.adapter->record_schema() };
    };

    // Matched pipeline PASSES.
    {
        auto err = xi::validate_record_pipeline({ stage(producer), stage(cons_ok) });
        CHECK(!err.has_value());
        if (err) std::fprintf(stderr, "  unexpected: %s\n", err->c_str());
        else std::printf("  matched contract: PASS\n");
    }
    // Wrong-type consumer CAUGHT.
    {
        auto err = xi::validate_record_pipeline({ stage(producer), stage(cons_bad) });
        CHECK(err.has_value());
        CHECK(err && err->find("score") != std::string::npos);
        std::printf("  wrong-type caught: %s\n", err ? err->c_str() : "(MISSED!)");
    }
    // Missing-field consumer CAUGHT.
    {
        auto err = xi::validate_record_pipeline({ stage(producer), stage(cons_missing) });
        CHECK(err.has_value());
        CHECK(err && err->find("width") != std::string::npos);
        std::printf("  missing-field caught: %s\n", err ? err->c_str() : "(MISSED!)");
    }
    // Order matters: consumer BEFORE producer → the field isn't produced yet.
    {
        auto err = xi::validate_record_pipeline({ stage(cons_ok), stage(producer) });
        CHECK(err.has_value());
        std::printf("  out-of-order caught: %s\n", err ? err->c_str() : "(MISSED!)");
    }

    if (g_failures == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
