//
// test_kv.cpp — xi::Kv container unit test (U2, docs/new_gen/16).
//
// In-memory + serialization properties of the post-Record script-state store:
//   1. typed set/get/has/type_of/erase/clear round-trip
//   2. serialize -> parse -> serialize is byte-identical, and the bytes are
//      INSERTION-ORDER INDEPENDENT (sorted keys = deterministic encoding)
//   3. the canonical gate on set_mp: canonical Writer bytes pass
//      byte-identical; ext / duplicate-key / non-string-key / malformed
//      bytes are REFUSED with the store untouched
//   4. parse: foreign compact widths are accepted AND normalized to the
//      canonical profile; hostile input (trailing bytes, non-map top level,
//      nil values, uint beyond int64) is refused ALL-OR-NOTHING
//   5. NaN survives the round-trip as the canonical quiet-NaN (Writer ruling)
//   6. thunk conventions: get=0 on empty, grow-and-retry, set refusal, and
//      kv_change decline/migrate paths (the bodies the real exports wrap)
//
// Header-only under test: links nothing (like test_mp).
//

#include <xi/xi_kv.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(expr)                                                  \
    do {                                                             \
        if (!(expr)) {                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                 \
                __FILE__, __LINE__, #expr);                          \
            std::abort();                                            \
        }                                                            \
    } while (0)

#define SECTION(name) std::fprintf(stderr, "-- %s\n", name)

using xi::Kv;
namespace mp = xi::mp;

int main() {
    std::fprintf(stderr, "=== test_kv ===\n");

    // ------------------------------------------------------------- SECTION 1
    SECTION("1: typed round-trip in memory");
    {
        Kv kv;
        CHECK(kv.empty());
        CHECK(kv.set_i64("count", 42));
        CHECK(kv.set_f64("ratio", 2.5));
        CHECK(kv.set_bool("armed", true));
        CHECK(kv.set_str("label", "blob-A"));
        const uint8_t raw[] = {0xde, 0xad, 0x00, 0xbe, 0xef};   // embedded NUL
        CHECK(kv.set_bin("raw", raw, sizeof raw));
        mp::Writer w;
        w.array(2); w.int_(3); w.int_(4);
        CHECK(kv.set_mp("pts", w));

        CHECK(kv.size() == 6);
        CHECK(kv.get_i64("count") == 42);
        CHECK(kv.get_f64("ratio") == 2.5);
        CHECK(kv.get_f64("count") == 42.0);          // widening i64 read
        CHECK(kv.get_bool("armed"));
        CHECK(kv.get_str("label") == "blob-A");
        const mp::Bytes* b = kv.get_bin("raw");
        CHECK(b && b->size() == sizeof raw && std::memcmp(b->data(), raw, sizeof raw) == 0);
        const mp::Bytes* m = kv.get_mp("pts");
        CHECK(m && *m == w.bytes());                 // canonical input passed byte-identical

        // wrong-type reads fall to the default (Record precedent)
        CHECK(kv.get_i64("label", -1) == -1);
        CHECK(kv.get_str("count", "d") == "d");
        CHECK(kv.get_bin("pts") == nullptr);
        CHECK(kv.get_mp("raw") == nullptr);
        // strict path
        CHECK(kv.has("count") && kv.type_of("count") == Kv::Type::I64);
        CHECK(kv.type_of("pts") == Kv::Type::Mp);
        CHECK(!kv.has("absent"));
        CHECK(kv.get_i64("absent", 7) == 7);

        CHECK(kv.erase("raw"));
        CHECK(!kv.erase("raw"));
        CHECK(!kv.has("raw"));
        CHECK(kv.keys().size() == 5);
        kv.clear();
        CHECK(kv.empty());
    }

    // ------------------------------------------------------------- SECTION 2
    SECTION("2: deterministic serialization (sorted keys, order-independent)");
    {
        Kv a, b;
        a.set_i64("zeta", 1); a.set_str("alpha", "x"); a.set_bool("mid", false);
        b.set_bool("mid", false); b.set_i64("zeta", 1); b.set_str("alpha", "x");
        mp::Bytes ba = a.serialize(), bb = b.serialize();
        CHECK(ba == bb);                              // insertion order irrelevant

        Kv c;
        CHECK(c.parse(ba.data(), ba.size()));
        CHECK(c.size() == 3 && c.get_i64("zeta") == 1 && c.get_str("alpha") == "x");
        CHECK(c.serialize() == ba);                   // parse -> serialize identity

        // the serialized form validates under the strict reject-all-ext policy
        CHECK(mp::validate(ba.data(), ba.size()) == mp::Status::Ok);
    }

    // ------------------------------------------------------------- SECTION 3
    SECTION("3: canonical gate on set_mp (refusals leave the store untouched)");
    {
        Kv kv;
        kv.set_i64("keep", 1);

        // ext value (a forged pool handle shape) — REFUSED
        const uint8_t ext[] = {0xd4, 0x01, 0xff};                 // fixext1
        CHECK(!kv.set_mp("bad", ext, sizeof ext));
        // duplicate-key map — REFUSED (ruling 5)
        const uint8_t dup[] = {0x82, 0xa1, 'k', 0x01, 0xa1, 'k', 0x02};
        CHECK(!kv.set_mp("bad", dup, sizeof dup));
        // non-string map key — REFUSED (ruling 2)
        const uint8_t ik[] = {0x81, 0x01, 0x02};
        CHECK(!kv.set_mp("bad", ik, sizeof ik));
        // truncated — REFUSED
        const uint8_t trunc[] = {0x91};                            // fixarray(1), no element
        CHECK(!kv.set_mp("bad", trunc, sizeof trunc));
        // trailing bytes — REFUSED
        const uint8_t trail[] = {0x01, 0x02};
        CHECK(!kv.set_mp("bad", trail, sizeof trail));

        CHECK(!kv.has("bad"));
        CHECK(kv.size() == 1 && kv.get_i64("keep") == 1);          // untouched

        // foreign COMPACT widths are accepted and NORMALIZED to canonical
        const uint8_t compact[] = {0x92, 0x05, 0xa1, 'x'};         // [5, "x"] compact
        CHECK(kv.set_mp("norm", compact, sizeof compact));
        const mp::Bytes* n = kv.get_mp("norm");
        CHECK(n);
        mp::Writer canon;
        canon.array(2); canon.int_(5); canon.str("x");
        CHECK(*n == canon.bytes());                                // max-width re-encode
    }

    // ------------------------------------------------------------- SECTION 4
    SECTION("4: hostile parse input (all-or-nothing refusal)");
    {
        Kv kv;
        kv.set_i64("sentinel", 9);
        mp::Bytes good = kv.serialize();

        // non-map top level
        mp::Writer arr; arr.array(1); arr.int_(1);
        CHECK(!kv.parse(arr.bytes().data(), arr.bytes().size()));
        // nil value (never emitted for a Kv)
        mp::Writer nil; nil.map(1); nil.key("k"); nil.nil();
        CHECK(!kv.parse(nil.bytes().data(), nil.bytes().size()));
        // uint beyond int64 (never ours)
        mp::Writer big; big.map(1); big.key("k"); big.uint_(0x8000000000000000ull);
        CHECK(!kv.parse(big.bytes().data(), big.bytes().size()));
        // trailing bytes after the map
        mp::Bytes trail = good; trail.push_back(0x01);
        CHECK(!kv.parse(trail.data(), trail.size()));
        // empty / null
        CHECK(!kv.parse(nullptr, 5));
        CHECK(!kv.parse(good.data(), 0));

        // every refusal above left the store untouched
        CHECK(kv.size() == 1 && kv.get_i64("sentinel") == 9);

        // foreign compact TOP-LEVEL map is accepted (canonicalized on entry):
        // {"a": 1} in fixmap/fixstr/fixint form
        const uint8_t compact[] = {0x81, 0xa1, 'a', 0x01};
        CHECK(kv.parse(compact, sizeof compact));
        CHECK(kv.size() == 1 && kv.get_i64("a") == 1);             // replaced wholesale
        // and re-serializes CANONICAL (map32+str32+int64), not compact
        mp::Bytes rt = kv.serialize();
        CHECK(rt.size() > sizeof compact);
        CHECK(rt[0] == 0xdf);                                      // map32
    }

    // ------------------------------------------------------------- SECTION 5
    SECTION("5: NaN normalizes to the canonical quiet-NaN and survives");
    {
        Kv kv;
        kv.set_f64("nan", std::nan("0x7ff"));
        kv.set_f64("ninf", -INFINITY);
        mp::Bytes b = kv.serialize();
        Kv back;
        CHECK(back.parse(b.data(), b.size()));
        CHECK(std::isnan(back.get_f64("nan", 0.0)));
        CHECK(back.get_f64("ninf", 0.0) == -INFINITY);
        CHECK(back.serialize() == b);                              // still deterministic
    }

    // ------------------------------------------------------------- SECTION 6
    SECTION("6: host-boundary thunk bodies (the ones the real exports wrap)");
    {
        // NOTE: these mutate the process-global xi::kv() — this section runs
        // last so earlier sections see a pristine store.
        using namespace xi::detail;
        CHECK(kv_get_thunk(nullptr, 0) == 0);                      // empty store => 0
        {
            std::lock_guard<std::mutex> lk(xi::kv_mutex());
            xi::kv().set_i64("count", 3);
            xi::kv().set_str("who", "kv");
        }
        int need = kv_get_thunk(nullptr, 0);
        CHECK(need < 0);                                           // grow-and-retry
        std::vector<uint8_t> buf((size_t)(-need));
        int n = kv_get_thunk(buf.data(), (int)buf.size());
        CHECK(n == -need);

        // set refusal: garbage bytes leave the live store untouched
        const uint8_t junk[] = {0xc1, 0x00};
        CHECK(kv_set_thunk(junk, sizeof junk) == -1);
        CHECK(xi::kv().get_i64("count") == 3);

        // round-trip through the boundary bytes
        {
            std::lock_guard<std::mutex> lk(xi::kv_mutex());
            xi::kv().clear();
        }
        CHECK(kv_set_thunk(buf.data(), n) == 0);
        CHECK(xi::kv().get_i64("count") == 3 && xi::kv().get_str("who") == "kv");

        // kv_change: no migrator -> decline
        CHECK(kv_change_thunk(buf.data(), n, 1, 2, nullptr, 0) == 0);
        // migrator registered -> re-shape carried forward
        xi::set_kv_migrate([](const Kv& old, int from, int to) -> std::optional<Kv> {
            Kv out;
            out.set_i64("frames", old.get_i64("count", 0));
            out.set_i64("migrated_from", from);
            out.set_i64("migrated_to", to);
            return out;
        });
        int mneed = kv_change_thunk(buf.data(), n, 1, 2, nullptr, 0);
        CHECK(mneed < 0);
        std::vector<uint8_t> mbuf((size_t)(-mneed));
        int mn = kv_change_thunk(buf.data(), n, 1, 2, mbuf.data(), (int)mbuf.size());
        CHECK(mn == -mneed);
        Kv migrated;
        CHECK(migrated.parse(mbuf.data(), (size_t)mn));
        CHECK(migrated.get_i64("frames") == 3);
        CHECK(migrated.get_i64("migrated_from") == 1 && migrated.get_i64("migrated_to") == 2);
        // migrator declining -> 0
        xi::set_kv_migrate([](const Kv&, int, int) -> std::optional<Kv> {
            return std::nullopt;
        });
        CHECK(kv_change_thunk(buf.data(), n, 1, 2, nullptr, 0) == 0);
        xi::set_kv_migrate(nullptr);
    }

    std::fprintf(stderr, "\nALL TESTS PASSED\n");
    return 0;
}
