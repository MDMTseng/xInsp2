#pragma once
//
// xi_pack_contract.hpp — the pack-plane error-path + provenance contract
// (polaris2 U1, docs/new_gen/15-pack-fault-semantics.md).
//
// ONE home for the reserved `$`-prefixed pack keys and the vtable-level
// helpers every side of the seam shares:
//
//   * the plugin SDK (xi_abi.hpp: PackOut::fault/src/prov, PackIn readers,
//     the pack_door_abi provenance stamp),
//   * the script SDK (xi_use.hpp: ScriptPack::is_fault()/fault_reason()/...,
//     xi_script_pack.hpp: ScriptPackBuilder::fault()/src()),
//   * the host funnel (service_sinks.cpp use_pack_process_cb: the fault
//     SHORT-CIRCUIT — propagate_fault below).
//
// Everything here speaks ONLY the process-stable xi_pack_v1 C vtable
// (xi_abi.h), so the same inline code is correct host-side, plugin-side and
// script-side (each binary reaches its own process's pack registry through
// the vtable it resolved — no cross-DLL singletons touched).
//
// THE CONTRACT IN ONE PARAGRAPH (doc 15): a fault is a NORMAL sealed pack
// carrying "$fault" (str reason code; xi::contract codes for contract
// failures, free-form producer codes otherwise) — never XI_PACK_NULL, which
// stays reserved for hard internal failure. Sealed packs are IMMUTABLE, so
// new information always means a NEW pack: producer identity ("$src") and
// the hop chain ("$prov", '/'-joined, oldest→newest) are stamped BEFORE seal
// by the producing side (pack_door_abi / emit), and the host funnel
// PROPAGATES a fault input by minting a new fault pack (original reason +
// this hop appended) WITHOUT running the plugin — the pack mirror of the
// Record path's `if (input.is_na()) return Record::na(reason).set_src(name)`.
//

#include "xi_abi.h"

#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace xi {
namespace pack_contract {

// --- Reserved pack keys ------------------------------------------------------
// Fail-loud, Pack-shaped: a contract failure is a NORMAL sealed pack carrying
// these top-level entries (so the caller always gets a pack to route to a
// verdict), mirroring xi::contract's $fault Record entry. Reuse the SAME reason
// codes as xi_contract.hpp (missing_input / wrong_type / schema_mismatch). See
// contract/canonical-profile-notes.md § "Pack-shaped fail-loud".
inline constexpr const char* kFault       = "$fault";        // str: reason code
inline constexpr const char* kFaultKey    = "$fault_key";    // str: offending key
inline constexpr const char* kFaultDetail = "$fault_detail"; // str: human detail
// U1 provenance (doc 15): who produced this pack, and through which hops.
inline constexpr const char* kSrc  = "$src";   // str: immediate producer (instance name)
inline constexpr const char* kProv = "$prov";  // str: hop chain, '/'-joined, oldest→newest
// Ordering identity (the established $seq convention). Named here because
// propagate_fault copies it forward so a propagated fault stays correlatable
// with its frame.
inline constexpr const char* kSeq = "$seq";
// UI/sink routing channel (expose + record_save/replay). A source/script stamps
// $channel on an emitted pack to name the display/record channel it belongs to
// (the v12 replacement for the deleted xi::Record::kChannelKey). str.
inline constexpr const char* kChannel = "$channel";
// Streaming via chunking (docs/new_gen/18-stream-chunking-convention.md): a
// logical payload too large / too long-lived for one sealed pack travels as a
// SEQUENCE of ordinary sealed packs on one lane — same "$stream" id, dense
// 0-based "$part", "$eof" present-and-true on the LAST chunk only. Pure
// CONVENTION: the host never buffers, reassembles or stamps these; the
// consumer owns the (bounded) reassembly window. A "$fault" pack carrying the
// stream's "$stream" id poisons the WHOLE stream (doc 15's one poison marker).
inline constexpr const char* kStream = "$stream"; // i64: producer-chosen stream id
inline constexpr const char* kPart   = "$part";   // i64: 0-based dense chunk index
inline constexpr const char* kEof    = "$eof";    // bool: true on the LAST chunk only
inline constexpr char kProvSep = '/';

// --- reserved-key predicates -------------------------------------------------
// THE single definition of "reserved key": the pack contract reserves the '$'
// prefix — every key above ($fault/$fault_key/$fault_detail/$src/$prov/$seq/
// $channel/$stream/$part/$eof) lives in that namespace, and future contract
// keys will too. A consumer asking "is this key the contract's or the
// producer's?" asks HERE, not with a hand-rolled prefix check.
inline constexpr bool is_reserved(std::string_view key) {
    return !key.empty() && key.front() == '$';
}

// The header-LIFT subset of the reserved keys: exactly the two entries the
// XEX1-v3 frame lifts into its top-level channel/seq fields (doc 07), so every
// generic dump walk (expose's wire push, record_save's disk persist) skips
// these — and ONLY these — as entries. The other reserved keys ($fault/$src/
// $prov/...) are deliberately NOT skipped: a fault pack must stay visible on
// wire and disk. ONE definition so the skip-set cannot drift between sinks.
inline constexpr bool is_lifted(std::string_view key) {
    return key == std::string_view(kChannel) || key == std::string_view(kSeq);
}

// --- vtable-level reads (host / plugin / script alike) -----------------------

// Borrowed str entry via the door; nullopt on absent key / wrong type / null
// vtable. The view is valid while the caller's ref on `h` is.
inline std::optional<std::string_view> str_entry(const xi_pack_v1* fi,
                                                 xi_pack_handle h,
                                                 const char* key) {
    if (!fi || h == XI_PACK_NULL || !fi->get_str) return std::nullopt;
    const char* p = nullptr; int32_t n = 0;
    if (fi->get_str(h, key, &p, &n) != 1) return std::nullopt;
    return std::string_view(p ? p : "", n > 0 ? (size_t)n : 0);
}

// A pack is a fault iff it carries a "$fault" entry (any type — a producer
// that mistyped it still poisoned the pack; fail loud, don't launder it).
inline bool is_fault(const xi_pack_v1* fi, xi_pack_handle h) {
    return fi && h != XI_PACK_NULL && fi->tag_of && fi->tag_of(h, kFault) >= 0;
}

// --- provenance chain --------------------------------------------------------

// The chain a pack contributes to its consumer: its own "$prov" if present
// (it already accumulated hops), else its "$src" (a single-hop origin), else
// empty (an unattributed pack — script-built, or a pre-U1 producer).
inline std::string prov_parent(const xi_pack_v1* fi, xi_pack_handle h) {
    if (auto p = str_entry(fi, h, kProv)) return std::string(*p);
    if (auto s = str_entry(fi, h, kSrc))  return std::string(*s);
    return {};
}

// Append one hop: "" + "det0" -> "det0"; "cam0/det0" + "det1" -> "cam0/det0/det1".
inline std::string prov_append(std::string parent, std::string_view hop) {
    if (parent.empty()) return std::string(hop);
    parent.push_back(kProvSep);
    parent.append(hop);
    return parent;
}

// --- the PROPAGATE primitive (doc 15 §short-circuit) -------------------------
//
// Mint the NEW sealed pack a fault input flows through a door as, WITHOUT
// running the plugin: the original "$fault"/"$fault_key"/"$fault_detail"
// (and "$seq", so the fault stays correlatable with its frame; and the stream
// identity "$stream"/"$part"/"$eof" when present, so a mid-stream fault keeps
// poisoning its stream after a hop — F1/doc 18) are copied,
// "$src" becomes this hop, and the hop is appended to "$prov". Precondition:
// is_fault(fi, in) — but a torn/mistyped "$fault" entry still yields a fault
// pack (reason "fault") rather than silently laundering the poison.
//
// Returns a fresh sealed handle (refcount 1, CALLER owns) or XI_PACK_NULL on
// a host without the pack plane. Cheap by design: four small str entries +
// one i64 in a fresh arena — no image/bin payload is carried (a poisoned
// frame's payload is exactly what downstream must NOT consume).
inline xi_pack_handle propagate_fault(const xi_pack_v1* fi, xi_pack_handle in,
                                      const char* hop) {
    if (!fi || !fi->builder_new || !fi->builder_add_str || !fi->builder_seal)
        return XI_PACK_NULL;
    std::string_view hop_sv(hop ? hop : "");
    xi_pack_builder b = fi->builder_new();

    auto reason = str_entry(fi, in, kFault);
    std::string_view rsv = reason ? *reason : std::string_view("fault");
    fi->builder_add_str(b, kFault, rsv.data(), (int32_t)rsv.size());
    if (auto v = str_entry(fi, in, kFaultKey))
        fi->builder_add_str(b, kFaultKey, v->data(), (int32_t)v->size());
    if (auto v = str_entry(fi, in, kFaultDetail))
        fi->builder_add_str(b, kFaultDetail, v->data(), (int32_t)v->size());
    if (fi->get_i64 && fi->builder_add_i64) {
        int64_t seq = 0;
        if (fi->get_i64(in, kSeq, &seq) == 1) fi->builder_add_i64(b, kSeq, seq);
        // F1 (doc 18/15): a mid-stream fault must keep its STREAM IDENTITY, else
        // a propagated fault loses the "$stream" id the consumer keys its poison
        // marker on — the poison silently stops poisoning that stream. Carry
        // "$stream"/"$part" (i64) and "$eof" (bool) forward when present, exactly
        // as "$seq" is carried, so a fault that flows through a funnel hop still
        // names the stream it belongs to (doc 15's one poison marker).
        int64_t stream = 0;
        if (fi->get_i64(in, kStream, &stream) == 1) fi->builder_add_i64(b, kStream, stream);
        int64_t part = 0;
        if (fi->get_i64(in, kPart, &part) == 1) fi->builder_add_i64(b, kPart, part);
    }
    if (fi->get_bool && fi->builder_add_bool) {
        int32_t eof = 0;
        if (fi->get_bool(in, kEof, &eof) == 1) fi->builder_add_bool(b, kEof, eof);
    }
    std::string chain = prov_append(prov_parent(fi, in), hop_sv);
    fi->builder_add_str(b, kSrc, hop_sv.data(), (int32_t)hop_sv.size());
    fi->builder_add_str(b, kProv, chain.data(), (int32_t)chain.size());
    return fi->builder_seal(b);
}

} // namespace pack_contract
} // namespace xi
