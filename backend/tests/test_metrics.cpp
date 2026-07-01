//
// test_metrics.cpp — OQ-7a observability export (core_fix_plan §21).
//
// Drives the MetricsRegistry the way run_one_inspection does (record_frame per
// completed inspection, from many threads) and asserts the minimal surface:
//
//   1. Counters advance: frames_total == ok + error, and each matches what we fed.
//   2. Latency histogram: frames land in the right fixed bucket (incl. the +inf
//      overflow), and the buckets sum to frames_total.
//   3. Snapshot serializes: snapshot_json produces a blob that PARSES as JSON and
//      carries the documented keys/values (frames_*, latency_ms.{count,sum,mean},
//      the bucket array with an "inf" overflow entry).
//   4. Concurrency smoke: N threads recording concurrently lose no increments
//      (lock-free counters) — heavy under XINSP2_STRESS_SCALE.
//
// Perf gates are Debug-slow (OQ-8) — this asserts only that metrics MOVE, never
// a wall-clock number.
//
#include <xi/xi_metrics.hpp>
#include "yyjson.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

static int fails = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                    \
                __FILE__, __LINE__, #expr);                             \
            ++fails;                                                    \
        }                                                               \
    } while (0)
#define SECTION(name) std::fprintf(stderr, "\n[section] %s\n", name)

static int stress_scale() {
    const char* e = std::getenv("XINSP2_STRESS_SCALE");
    int v = e ? std::atoi(e) : 1;
    return v > 0 ? v : 1;
}

int main() {
    using xi::MetricsRegistry;
    auto& m = MetricsRegistry::instance();

    SECTION("counters advance");
    {
        m.reset();
        CHECK(m.frames_total() == 0);
        // 5 ok frames + 3 error frames.
        for (int i = 0; i < 5; ++i) m.record_frame(3.0, /*ok=*/true);
        for (int i = 0; i < 3; ++i) m.record_frame(3.0, /*ok=*/false);
        CHECK(m.frames_total() == 8);
        CHECK(m.frames_ok()    == 5);
        CHECK(m.frames_error() == 3);
    }

    SECTION("latency histogram buckets");
    {
        m.reset();
        // Edges (ms): 0.5,1,2,5,10,20,50,100,200,500,1000,2000,5000, then +inf.
        m.record_frame(0.2, true);      // -> bucket 0 (le 0.5)
        m.record_frame(0.5, true);      // -> bucket 0 (le 0.5, inclusive)
        m.record_frame(3.0, true);      // -> bucket 3 (le 5)
        m.record_frame(3.0, true);      // -> bucket 3 (le 5)
        m.record_frame(150.0, true);    // -> bucket 8 (le 200)
        m.record_frame(999999.0, true); // -> +inf overflow (index kBuckets)
        CHECK(m.frames_total() == 6);
        CHECK(m.bucket(0) == 2);
        CHECK(m.bucket(3) == 2);
        CHECK(m.bucket(8) == 1);
        CHECK(m.bucket(MetricsRegistry::kBuckets) == 1);   // +inf
        // Buckets partition every frame — they must sum to frames_total.
        uint64_t sum = 0;
        for (std::size_t i = 0; i <= MetricsRegistry::kBuckets; ++i) sum += m.bucket(i);
        CHECK(sum == m.frames_total());
    }

    SECTION("snapshot serializes + parses");
    {
        m.reset();
        m.record_frame(1.0, true);
        m.record_frame(4.0, true);
        m.record_frame(10.0, false);
        std::string blob;
        m.snapshot_json(blob);
        CHECK(!blob.empty());

        yyjson_doc* doc = yyjson_read(blob.c_str(), blob.size(), 0);
        CHECK(doc != nullptr);
        if (doc) {
            yyjson_val* root = yyjson_doc_get_root(doc);
            CHECK(yyjson_is_obj(root));
            CHECK(yyjson_get_uint(yyjson_obj_get(root, "frames_total")) == 3);
            CHECK(yyjson_get_uint(yyjson_obj_get(root, "frames_ok"))    == 2);
            CHECK(yyjson_get_uint(yyjson_obj_get(root, "frames_error")) == 1);

            yyjson_val* lat = yyjson_obj_get(root, "latency_ms");
            CHECK(yyjson_is_obj(lat));
            CHECK(yyjson_get_uint(yyjson_obj_get(lat, "count")) == 3);
            // mean = (1+4+10)/3 = 5.0 ms; sum = 15.0.
            double sum_ms  = yyjson_get_real(yyjson_obj_get(lat, "sum_ms"));
            double mean_ms = yyjson_get_real(yyjson_obj_get(lat, "mean_ms"));
            CHECK(sum_ms  > 14.9 && sum_ms  < 15.1);
            CHECK(mean_ms > 4.9  && mean_ms < 5.1);

            yyjson_val* buckets = yyjson_obj_get(lat, "buckets");
            CHECK(yyjson_is_arr(buckets));
            // kBuckets edges + 1 overflow entry.
            CHECK(yyjson_arr_size(buckets) == MetricsRegistry::kBuckets + 1);
            // Last entry is the "inf" overflow bucket.
            yyjson_val* last = yyjson_arr_get_last(buckets);
            yyjson_val* le   = yyjson_obj_get(last, "le");
            CHECK(yyjson_is_str(le));
            CHECK(std::string(yyjson_get_str(le)) == "inf");
            yyjson_doc_free(doc);
        }
    }

    SECTION("concurrency smoke (no lost increments)");
    {
        m.reset();
        const int threads = 8;
        const int per     = 5000 * stress_scale();
        std::vector<std::thread> ts;
        for (int t = 0; t < threads; ++t) {
            ts.emplace_back([&, t] {
                for (int i = 0; i < per; ++i)
                    m.record_frame((double)((i % 40) + 1) * 0.5, (i & 1) == 0);
            });
        }
        for (auto& th : ts) th.join();
        uint64_t expect = (uint64_t)threads * per;
        CHECK(m.frames_total() == expect);
        CHECK(m.frames_ok() + m.frames_error() == expect);
        uint64_t sum = 0;
        for (std::size_t i = 0; i <= MetricsRegistry::kBuckets; ++i) sum += m.bucket(i);
        CHECK(sum == expect);
    }

    if (fails) {
        std::fprintf(stderr, "\n%d CHECK(s) FAILED\n", fails);
        return 1;
    }
    std::fprintf(stderr, "\nOK — metrics counters/histogram/snapshot verified\n");
    return 0;
}
