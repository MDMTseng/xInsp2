//
// bench_record.cpp — Record serialization round-trip: cJSON vs yyjson vs
// MPack vs CWPack (+ a tight purpose-built msgpack as the floor).
//
// WHY: Record crosses every (now in-process) plugin boundary as a serialized
// blob. UseProxy::process does input.data_json() [print] -> ABI -> cJSON_Parse,
// then the same for the output — so one use().process() = 2 print + 2 parse of
// the whole tree, in-process. This bench measures that tax on a representative
// payload (a matcher result with N matches — the 10/50/150-object case) for
// several backends, to decide the wire format / DOM.
//
// Method: the Record's in-memory form is a cJSON tree (the source). For each
// backend we time, on that data:
//   serialize : produce wire bytes  (cJSON/yyjson -> JSON text; mpack/cwpack/
//               tight -> binary msgpack). For mpack/cwpack/tight we walk the
//               cJSON source and emit — exactly what record_to_c would do.
//   parse     : wire bytes -> a navigable form (cJSON/yyjson -> DOM; mpack ->
//               node DOM; cwpack/tight -> streaming walk, no DOM = its design).
// All numbers are best-of-R batches (min strips scheduler noise). Run manually.
//
#include "cJSON.h"
#include "yyjson.h"
#include "mpack.h"
extern "C" {
#include "cwpack.h"   // no extern "C" guard of its own
}

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ===========================================================================
// Tight purpose-built MessagePack over cJSON (the "floor": what a tailored path
// with no library generality would cost). map/array/str/double/bool/nil.
// ===========================================================================
namespace tight {
static inline void put8(std::string& o, uint8_t b) { o.push_back((char)b); }
static inline void be16(std::string& o, uint16_t v) { put8(o, v >> 8); put8(o, v); }
static inline void be32(std::string& o, uint32_t v) { put8(o, v >> 24); put8(o, v >> 16); put8(o, v >> 8); put8(o, v); }
static inline void be64(std::string& o, uint64_t v) { for (int s = 56; s >= 0; s -= 8) put8(o, (uint8_t)(v >> s)); }
static void enc_str(std::string& o, const char* s, size_t n) {
    if (n < 32) put8(o, 0xa0 | (uint8_t)n);
    else if (n < 256) { put8(o, 0xd9); put8(o, (uint8_t)n); }
    else { put8(o, 0xda); be16(o, (uint16_t)n); }
    o.append(s, n);
}
static void pack(const cJSON* n, std::string& o) {
    if (cJSON_IsObject(n)) {
        int c = cJSON_GetArraySize(n);
        if (c < 16) put8(o, 0x80 | (uint8_t)c); else { put8(o, 0xde); be16(o, (uint16_t)c); }
        for (const cJSON* k = n->child; k; k = k->next) { enc_str(o, k->string, std::strlen(k->string)); pack(k, o); }
    } else if (cJSON_IsArray(n)) {
        int c = cJSON_GetArraySize(n);
        if (c < 16) put8(o, 0x90 | (uint8_t)c); else { put8(o, 0xdc); be16(o, (uint16_t)c); }
        for (const cJSON* e = n->child; e; e = e->next) pack(e, o);
    } else if (cJSON_IsString(n)) enc_str(o, n->valuestring, std::strlen(n->valuestring));
    else if (cJSON_IsBool(n)) put8(o, cJSON_IsTrue(n) ? 0xc3 : 0xc2);
    else if (cJSON_IsNumber(n)) { double d = n->valuedouble; uint64_t b; std::memcpy(&b, &d, 8); put8(o, 0xcb); be64(o, b); }
    else put8(o, 0xc0);
}
struct Rd { const uint8_t* p; size_t i; };
static inline uint8_t r8(Rd& r) { return r.p[r.i++]; }
static inline uint16_t r16(Rd& r) { uint16_t v = (r.p[r.i] << 8) | r.p[r.i + 1]; r.i += 2; return v; }
static inline uint64_t r64(Rd& r) { uint64_t v = 0; for (int k = 0; k < 8; ++k) v = (v << 8) | r.p[r.i++]; return v; }
static void walk(Rd& r, double* acc) {  // streaming consume, sum doubles
    uint8_t b = r8(r);
    if (b < 0x80) return;
    if ((b & 0xf0) == 0x80 || b == 0xde) { size_t n = (b & 0xf0) == 0x80 ? (b & 15) : r16(r); for (size_t i = 0; i < n; ++i) { walk(r, acc); walk(r, acc); } }
    else if ((b & 0xf0) == 0x90 || b == 0xdc) { size_t n = (b & 0xf0) == 0x90 ? (b & 15) : r16(r); for (size_t i = 0; i < n; ++i) walk(r, acc); }
    else if ((b & 0xe0) == 0xa0) r.i += (b & 31);
    else if (b == 0xd9) r.i += r8(r);
    else if (b == 0xda) r.i += r16(r);
    else if (b == 0xcb) { uint64_t bits = r64(r); double d; std::memcpy(&d, &bits, 8); *acc += d; }
    // c0/c2/c3 -> nothing
}
} // namespace tight

// ===========================================================================
// cJSON-source -> each library's emit
// ===========================================================================
static yyjson_mut_val* cj2yy(yyjson_mut_doc* d, const cJSON* n) {
    if (cJSON_IsObject(n)) { yyjson_mut_val* o = yyjson_mut_obj(d); for (const cJSON* c = n->child; c; c = c->next) yyjson_mut_obj_add_val(d, o, c->string, cj2yy(d, c)); return o; }
    if (cJSON_IsArray(n))  { yyjson_mut_val* a = yyjson_mut_arr(d); for (const cJSON* c = n->child; c; c = c->next) yyjson_mut_arr_add_val(a, cj2yy(d, c)); return a; }
    if (cJSON_IsString(n)) return yyjson_mut_str(d, n->valuestring);
    if (cJSON_IsBool(n))   return yyjson_mut_bool(d, cJSON_IsTrue(n));
    if (cJSON_IsNumber(n)) return yyjson_mut_real(d, n->valuedouble);
    return yyjson_mut_null(d);
}
static void cj2mp(mpack_writer_t* w, const cJSON* n) {
    if (cJSON_IsObject(n)) { mpack_start_map(w, (uint32_t)cJSON_GetArraySize(n)); for (const cJSON* c = n->child; c; c = c->next) { mpack_write_cstr(w, c->string); cj2mp(w, c); } mpack_finish_map(w); }
    else if (cJSON_IsArray(n)) { mpack_start_array(w, (uint32_t)cJSON_GetArraySize(n)); for (const cJSON* c = n->child; c; c = c->next) cj2mp(w, c); mpack_finish_array(w); }
    else if (cJSON_IsString(n)) mpack_write_cstr(w, n->valuestring);
    else if (cJSON_IsBool(n)) mpack_write_bool(w, cJSON_IsTrue(n));
    else if (cJSON_IsNumber(n)) mpack_write_double(w, n->valuedouble);
    else mpack_write_nil(w);
}
static void cj2cw(cw_pack_context* pc, const cJSON* n) {
    if (cJSON_IsObject(n)) { cw_pack_map_size(pc, (uint32_t)cJSON_GetArraySize(n)); for (const cJSON* c = n->child; c; c = c->next) { cw_pack_str(pc, c->string, (uint32_t)std::strlen(c->string)); cj2cw(pc, c); } }
    else if (cJSON_IsArray(n)) { cw_pack_array_size(pc, (uint32_t)cJSON_GetArraySize(n)); for (const cJSON* c = n->child; c; c = c->next) cj2cw(pc, c); }
    else if (cJSON_IsString(n)) cw_pack_str(pc, n->valuestring, (uint32_t)std::strlen(n->valuestring));
    else if (cJSON_IsBool(n)) cw_pack_boolean(pc, cJSON_IsTrue(n));
    else if (cJSON_IsNumber(n)) cw_pack_double(pc, n->valuedouble);
    else cw_pack_nil(pc);
}
static double mp_sum(mpack_node_t n) {  // force full materialization
    mpack_type_t t = mpack_node_type(n);
    double s = 0;
    if (t == mpack_type_map) { size_t c = mpack_node_map_count(n); for (size_t i = 0; i < c; ++i) s += mp_sum(mpack_node_map_value_at(n, i)); }
    else if (t == mpack_type_array) { size_t c = mpack_node_array_length(n); for (size_t i = 0; i < c; ++i) s += mp_sum(mpack_node_array_at(n, i)); }
    else if (t == mpack_type_double) s += mpack_node_double(n);
    else if (t == mpack_type_float) s += mpack_node_float(n);
    return s;
}
static void cw_walk(cw_unpack_context* uc, double* acc) {
    cw_unpack_next(uc);
    switch (uc->item.type) {
        case CWP_ITEM_MAP:   { uint32_t n = uc->item.as.map.size;   for (uint32_t i = 0; i < n; ++i) { cw_walk(uc, acc); cw_walk(uc, acc); } break; }
        case CWP_ITEM_ARRAY: { uint32_t n = uc->item.as.array.size; for (uint32_t i = 0; i < n; ++i) cw_walk(uc, acc); break; }
        case CWP_ITEM_DOUBLE: *acc += uc->item.as.long_real; break;
        case CWP_ITEM_FLOAT:  *acc += uc->item.as.real; break;
        default: break;
    }
}

// matcher result: { "$src","match_count", "matches":[{name,x,y,angle,scale,score,flipped}*N] }
static cJSON* make_matches(int n) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "$src", "matcher");
    cJSON_AddNumberToObject(root, "match_count", n);
    cJSON* arr = cJSON_AddArrayToObject(root, "matches");
    for (int i = 0; i < n; ++i) {
        cJSON* m = cJSON_CreateObject();
        char nm[16]; std::snprintf(nm, sizeof(nm), "part_%d", i);
        cJSON_AddStringToObject(m, "name", nm);
        cJSON_AddNumberToObject(m, "x", 100.0 + i * 1.37);
        cJSON_AddNumberToObject(m, "y", 50.0 + i * 0.91);
        cJSON_AddNumberToObject(m, "angle", (i * 7) % 360 + 0.5);
        cJSON_AddNumberToObject(m, "scale", 1.0 + (i % 5) * 0.01);
        cJSON_AddNumberToObject(m, "score", 0.7 + (i % 30) * 0.01);
        cJSON_AddBoolToObject(m, "flipped", i & 1);
        cJSON_AddItemToArray(arr, m);
    }
    return root;
}

template <class F>
static double best_us(F&& op, int L = 2000, int R = 25) {
    using clk = std::chrono::steady_clock;
    op();
    double best = 1e30;
    for (int b = 0; b < R; ++b) {
        auto t0 = clk::now();
        for (int i = 0; i < L; ++i) op();
        auto t1 = clk::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / L;
        if (us < best) best = us;
    }
    return best;
}

int main() {
    std::printf("Record round-trip — cJSON vs yyjson vs MPack vs CWPack (per op, microseconds; min-of-batches)\n");
    std::printf("serialize = cJSON-tree -> wire ; parse = wire -> navigable form (cwpack/tight = streaming walk)\n\n");

    std::vector<uint8_t> cwbuf(1 << 20);

    for (int N : {10, 50, 150}) {
        cJSON* tree = make_matches(N);

        // --- pre-build wire forms (inputs for the parse benches) ---
        char* js = cJSON_PrintUnformatted(tree); std::string json_str = js; cJSON_free(js);

        yyjson_mut_doc* yd = yyjson_mut_doc_new(nullptr);
        yyjson_mut_doc_set_root(yd, cj2yy(yd, tree));
        size_t yylen = 0; char* yw = yyjson_mut_write(yd, 0, &yylen); size_t yy_bytes = yylen; free(yw);

        char* mpd; size_t mpsz; mpack_writer_t mw; mpack_writer_init_growable(&mw, &mpd, &mpsz);
        cj2mp(&mw, tree); mpack_writer_destroy(&mw);  // mpd/mpsz now own the bytes
        std::vector<uint8_t> mp_wire((uint8_t*)mpd, (uint8_t*)mpd + mpsz); free(mpd);

        cw_pack_context cwc; cw_pack_context_init(&cwc, cwbuf.data(), cwbuf.size(), nullptr);
        cj2cw(&cwc, tree); size_t cw_bytes = (size_t)(cwc.current - cwc.start);
        std::vector<uint8_t> cw_wire(cwbuf.data(), cwbuf.data() + cw_bytes);

        std::string tight_wire; tight::pack(tree, tight_wire);

        // --- timed ops ---
        double cj_ser = best_us([&]{ char* s = cJSON_PrintUnformatted(tree); cJSON_free(s); });
        double cj_par = best_us([&]{ cJSON* t = cJSON_Parse(json_str.c_str()); cJSON_Delete(t); });

        double yy_ser = best_us([&]{ size_t l; char* s = yyjson_mut_write(yd, 0, &l); free(s); });
        double yy_par = best_us([&]{ yyjson_doc* d = yyjson_read(json_str.data(), json_str.size(), 0); yyjson_doc_free(d); });

        double mp_ser = best_us([&]{ char* d; size_t s; mpack_writer_t w; mpack_writer_init_growable(&w, &d, &s); cj2mp(&w, tree); mpack_writer_destroy(&w); free(d); });
        double mp_par = best_us([&]{ mpack_tree_t t; mpack_tree_init_data(&t, (const char*)mp_wire.data(), mp_wire.size()); mpack_tree_parse(&t); volatile double s = mp_sum(mpack_tree_root(&t)); (void)s; mpack_tree_destroy(&t); });

        double cw_ser = best_us([&]{ cw_pack_context pc; cw_pack_context_init(&pc, cwbuf.data(), cwbuf.size(), nullptr); cj2cw(&pc, tree); });
        double cw_par = best_us([&]{ cw_unpack_context uc; cw_unpack_context_init(&uc, cw_wire.data(), (unsigned long)cw_wire.size(), nullptr); double a = 0; cw_walk(&uc, &a); volatile double s = a; (void)s; });

        double tt_ser = best_us([&]{ std::string o; tight::pack(tree, o); });
        double tt_par = best_us([&]{ tight::Rd r{(const uint8_t*)tight_wire.data(), 0}; double a = 0; tight::walk(r, &a); volatile double s = a; (void)s; });

        std::printf("=== N=%d ===\n", N);
        std::printf("  %-12s %10s %10s %12s %8s\n", "backend", "serialize", "parse", "round-trip", "bytes");
        auto row = [](const char* nm, double s, double p, size_t b) {
            std::printf("  %-12s %10.2f %10.2f %12.2f %8zu\n", nm, s, p, s + p, b);
        };
        row("cJSON",        cj_ser, cj_par, json_str.size());
        row("yyjson",       yy_ser, yy_par, yy_bytes);
        row("MPack",        mp_ser, mp_par, mp_wire.size());
        row("CWPack",       cw_ser, cw_par, cw_wire.size());
        row("tight(msgpk)", tt_ser, tt_par, tight_wire.size());
        std::printf("\n");

        yyjson_mut_doc_free(yd);
        cJSON_Delete(tree);
    }

    std::printf("round-trip x2 = the per-use().process() serialize+parse tax. Compare to ms-scale operator work.\n");
    return 0;
}
