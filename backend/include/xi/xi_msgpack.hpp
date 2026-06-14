#pragma once
//
// xi_msgpack.hpp — Record wire codec: cJSON tree <-> MessagePack bytes (CWPack).
//
// Replaces the cJSON JSON-text round-trip on the (in-process) plugin ABI seam.
// Measured ~21x faster than cJSON print+parse (backend/tests/bench_record.cpp).
// Phase 1 keeps cJSON as the in-memory DOM, so decode rebuilds a cJSON tree;
// see docs/design/wire-format-msgpack.md for the phasing.
//
// Two entry points, both total (never throw):
//   std::vector<uint8_t> cjson_to_msgpack(const cJSON* root)   // encode
//   cJSON*               msgpack_to_cjson(const uint8_t*, size_t) // decode (owns)
//
// Number policy: all cJSON numbers encode as msgpack float64 (the vision-data
// case); decode maps int/float/double back to cJSON numbers. Values > 2^53 lose
// precision exactly as they already do through cJSON (number == double).
//
#include "cJSON.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "cwpack.h"   // no extern "C" guard of its own
}

namespace xi {
namespace mp_detail {

// Growable pack context: cw_pack_context MUST be the first member so the
// overflow handler can recover the owning struct by pointer-casting.
struct GrowCtx {
    cw_pack_context pc;
    std::vector<uint8_t>* v;
};
inline int grow_handler(cw_pack_context* pc, unsigned long more) {
    GrowCtx* g = reinterpret_cast<GrowCtx*>(pc);
    size_t used = (size_t)(pc->current - pc->start);
    size_t need = used + more;
    size_t cap  = g->v->size();
    while (cap < need) cap = cap ? cap * 2 : 256;
    g->v->resize(cap);
    pc->start   = g->v->data();
    pc->current = pc->start + used;
    pc->end     = pc->start + g->v->size();
    return CWP_RC_OK;
}

inline void pack_node(cw_pack_context* pc, const cJSON* n) {
    if (cJSON_IsObject(n)) {
        cw_pack_map_size(pc, (uint32_t)cJSON_GetArraySize(n));
        for (const cJSON* c = n->child; c; c = c->next) {
            cw_pack_str(pc, c->string, (uint32_t)std::strlen(c->string));
            pack_node(pc, c);
        }
    } else if (cJSON_IsArray(n)) {
        cw_pack_array_size(pc, (uint32_t)cJSON_GetArraySize(n));
        for (const cJSON* c = n->child; c; c = c->next) pack_node(pc, c);
    } else if (cJSON_IsString(n)) {
        cw_pack_str(pc, n->valuestring, (uint32_t)std::strlen(n->valuestring));
    } else if (cJSON_IsBool(n)) {
        cw_pack_boolean(pc, cJSON_IsTrue(n) ? true : false);
    } else if (cJSON_IsNumber(n)) {
        cw_pack_double(pc, n->valuedouble);
    } else {
        cw_pack_nil(pc);   // null / invalid
    }
}

inline cJSON* unpack_node(cw_unpack_context* uc) {
    cw_unpack_next(uc);
    switch (uc->item.type) {
        case CWP_ITEM_MAP: {
            cJSON* o = cJSON_CreateObject();
            uint32_t n = uc->item.as.map.size;
            for (uint32_t i = 0; i < n; ++i) {
                cw_unpack_next(uc);   // key (str)
                std::string key((const char*)uc->item.as.str.start, uc->item.as.str.length);
                cJSON_AddItemToObject(o, key.c_str(), unpack_node(uc));
            }
            return o;
        }
        case CWP_ITEM_ARRAY: {
            cJSON* a = cJSON_CreateArray();
            uint32_t n = uc->item.as.array.size;
            for (uint32_t i = 0; i < n; ++i) cJSON_AddItemToArray(a, unpack_node(uc));
            return a;
        }
        case CWP_ITEM_STR: {
            std::string s((const char*)uc->item.as.str.start, uc->item.as.str.length);
            return cJSON_CreateString(s.c_str());
        }
        case CWP_ITEM_DOUBLE:           return cJSON_CreateNumber(uc->item.as.long_real);
        case CWP_ITEM_FLOAT:            return cJSON_CreateNumber(uc->item.as.real);
        case CWP_ITEM_POSITIVE_INTEGER: return cJSON_CreateNumber((double)uc->item.as.u64);
        case CWP_ITEM_NEGATIVE_INTEGER: return cJSON_CreateNumber((double)uc->item.as.i64);
        case CWP_ITEM_BOOLEAN:          return cJSON_CreateBool(uc->item.as.boolean ? 1 : 0);
        case CWP_ITEM_NIL:              return cJSON_CreateNull();
        default:                        return cJSON_CreateNull();
    }
}

} // namespace mp_detail

// Encode a cJSON tree to MessagePack bytes. Empty vector if root is null.
inline std::vector<uint8_t> cjson_to_msgpack(const cJSON* root) {
    std::vector<uint8_t> v;
    if (!root) return v;
    v.resize(256);
    mp_detail::GrowCtx g;
    g.v = &v;
    cw_pack_context_init(&g.pc, v.data(), (unsigned long)v.size(), mp_detail::grow_handler);
    mp_detail::pack_node(&g.pc, root);
    if (g.pc.return_code != CWP_RC_OK) { v.clear(); return v; }
    v.resize((size_t)(g.pc.current - g.pc.start));
    return v;
}

// Decode MessagePack bytes to a NEW cJSON tree (caller owns → cJSON_Delete).
// Returns an empty object on null/empty/malformed input (total, never throws).
inline cJSON* msgpack_to_cjson(const uint8_t* data, size_t len) {
    if (!data || len == 0) return cJSON_CreateObject();
    cw_unpack_context uc;
    cw_unpack_context_init(&uc, data, (unsigned long)len, nullptr);
    cJSON* root = mp_detail::unpack_node(&uc);
    if (uc.return_code != CWP_RC_OK || !root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return cJSON_CreateObject();
    }
    return root;
}

} // namespace xi
