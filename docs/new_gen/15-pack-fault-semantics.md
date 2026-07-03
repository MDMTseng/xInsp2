# 15 — Pack-plane fault, provenance and short-circuit semantics (U1)

Status: **DECIDED + LANDED** (polaris2, branch `polaris2/u1-pack-fault-semantics`).
Closes the propagate/provenance half of **Unscheduled U1** (doc 12 §Unscheduled,
doc 10 Gate-P2 residual #1) — the pack-plane error-path story that gated the
error patterns of `fixturing_demo` / `io_stress` / `graph_demo` (matrix rows
B3/B4). The `_io`-style **typed build/extract helpers** named in the same U1
bundle are NOT this doc's scope and remain open (see §Deliberately out below).

This is a DECISION document, not an options survey. One home for the code:
`backend/include/xi/xi_pack_contract.hpp` (`xi::pack_contract`).

---

## 1. The problem it solves

The Record world has a complete error-path discipline: `Record::na(reason)` /
`is_na()` / `na_reason()` poison values, `$src`/`$prov` provenance, and the NA
short-circuit in `xi::use()` — a poison input never enters plugin code, the NA
(with its reason) flows through, `$src` names the hop
(`UseProxy::process(Record)`, `xi_use.hpp`).

The pack world had only the fail-loud SEED: a door that hits a CONTRACT
failure returns a normal sealed pack carrying `$fault` (never `XI_PACK_NULL`,
which stays reserved for hard internal failure). Scripts could READ a fault;
nothing PROPAGATED it, nothing recorded who produced a pack, and a fault fed
into the next door would happily run the plugin on poisoned input.

## 2. The decisions

### D1 — ONE poison marker: `$fault`. No pack-plane `$na`.

The Record plane splits "not available" (`$na`) from "contract failure"
(nested `$fault` object) — a JSON-plane legacy where the NA marker doubles as
the empty-result shape. The pack plane deliberately does NOT copy that split:
**a pack is poisoned iff it carries a top-level `$fault` entry**, whatever the
reason. "Frame never arrived" and "required key missing" differ only in the
reason code. One marker means one check (`is_fault()`), one propagation rule,
and no laundering path between two poison vocabularies.

### D2 — The fault schema (reserved top-level entries, all flat, canonical)

| Key | Type | Meaning |
|---|---|---|
| `$fault` | str, required on a fault pack, non-empty | Reason code. The `xi::contract` codes (`missing_input` / `wrong_type` / `schema_mismatch`) are the blessed vocabulary for contract-shaped failures; producer codes are free-form (`frame_timeout`, `bad_replay_file`, …). |
| `$fault_key` | str, optional | The offending entry key. |
| `$fault_detail` | str, optional | Human message. |
| `$src` | str | **NEW.** The immediate producer: the instance whose seal minted this pack (or the funnel hop that propagated it). Pack sibling of `Record::set_src()`. |
| `$prov` | str | **NEW.** The hop chain: instance names joined by `/`, oldest→newest (`cam0/det0/det1`). Deliberately a flat string, not a nested mp array — zero decode cost, greppable in dumps, and honest about being a breadcrumb, not a database. (Record's `$prov` is a per-field map; the pack plane's is a per-PACK chain — packs are sealed wholes, fields don't have independent origins.) |

`$fault`/`$fault_key`/`$fault_detail` predate this doc (`PackOut::fault`);
`$src`/`$prov` are minted here. Constants + helpers: `xi::pack_contract`
(`xi_pack_contract.hpp`); prose registry: `contract/canonical-profile-notes.md`
§"Pack-shaped fail-loud"; authored-facing table: `docs/reference/data-types.md`.
Reserved `$`-keys stay out of every plugin's declared schema keyset.

### D3 — Provenance is stamped PRODUCER-SIDE, before seal

Sealed packs are immutable — nobody stamps into an existing pack, and the push
path keeps its "no host stamping, byte-identical dump" guarantee untouched. So
identity is written where the pack is BUILT:

* **Door outputs** — `Plugin::pack_door_abi` (the `XI_PLUGIN_PACK_DOOR` glue,
  `xi_abi.hpp`) stamps every NON-EMPTY door output after the plugin's
  `process(PackIn&, PackOut&)` returns and before `seal()`:
  `$src` = the instance name, `$prov` = the INPUT's chain + this hop.
  Zero plugin-author effort; result packs and plugin-minted `$fault` packs get
  identity from birth. A plugin that calls the explicit `PackOut::src()` /
  `prov()` setters wins (a forwarding router preserving someone else's
  attribution) — use the setters, not raw `str("$src", …)`: only the setters
  suppress the automatic stamp. An UNTOUCHED `PackOut` seals EMPTY on
  purpose (`PackOut::touched()` gates the stamp): the empty pack is the
  door's absence sentinel, the pack mirror of the Record path's empty `{}` —
  provenance rides data; stamping identity onto nothing would turn absence
  into presence (asserted by `config_swap_probe`'s pack tests).
* **Emitted packs** — `Plugin::emit(PackOut&&)` stamps NOTHING. A measured
  decision, not an omission: an emitted pack's entry set is the producer's
  published contract — `record_replay` re-emits disk dumps BYTE-IDENTICAL
  (the E3 lossless loop, doc 13), and a gatherer's pack shape
  (`{left,right,seq}`, `synced_stereo`) must not grow surprise entries. A
  source that wants origin attribution calls `out.src(name())` explicitly;
  either way the chain materialises at the first door hop (the parent rule
  below turns a lone `$src` into the chain root, and an unattributed pack
  simply starts its chain there).
* **Script-built packs** — NO automatic identity (a script is not an
  instance). `ScriptPackBuilder::src()` is available for explicit attribution;
  downstream hops still accumulate on `$prov` as the pack propagates.
* **Prebuilt third-party plugins** compiled before this glue simply don't
  stamp — producer-side by design; their packs are "unattributed" and the
  chain starts at the first post-U1 hop.

The parent-chain rule (`pack_contract::prov_parent`): a pack contributes its
`$prov` if present, else its `$src`, else nothing. Appending is
`prov_append(parent, hop)` — `"" + det0 → "det0"`,
`"cam0/det0" + det1 → "cam0/det0/det1"`.

### D4 — The PROPAGATE contract: the host funnel short-circuits

`use(name).process(faultPack)` NEVER runs the plugin. The host funnel
(`use_pack_process_cb`, `backend/src/service_sinks.cpp`) checks
`pack_contract::is_fault(in)` FIRST — before the instance lookup, exactly
where the Record path's `if (input.is_na()) return
Record::na(reason).set_src(name)` sits (before name resolution: poison
propagates even through a typo'd, quarantined or door-less name; the frame's
failure is already explained by the carried reason, and the plugin must not
run either way). It returns rc 0 with a NEW sealed pack minted by
`pack_contract::propagate_fault(fi, in, hop)`:

* `$fault` / `$fault_key` / `$fault_detail` — copied from the input (a
  torn/mistyped `$fault` entry still yields reason `"fault"`, never a
  laundered non-fault);
* `$seq` — copied if present, so the fault stays correlatable with its frame;
* `$src` = this hop; `$prov` = input chain + this hop;
* **nothing else** — a poisoned frame's payload (images, bins, results) is
  exactly what downstream must not consume, so it is not carried. Cheap by
  construction: a handful of small str entries in a fresh arena.

The propagated pack is the call's normal result (the script's ScriptPack owns
it) — poison FLOWS, with an audit trail, until something routes it to a
verdict. The short-circuit lives in ONE function shared by the service funnel
and the test harness's funnel mirror, so tests exercise the same code the
service runs. The SDK side (`UseProxy::process(ScriptPack)`) is deliberately
NOT a second short-circuit point: one funnel, one behavior, every caller
covered. The empty-input rule is unchanged (empty in → empty out, no host
round-trip). `use(sink).push(faultPack)` also unchanged: push is fire-and-
forget delivery AS-IS — a sink SEES the fault pack (and can dump it, `$fault`
and all); nothing to propagate.

### D5 — The read/write surface

Script side (`ScriptPack`, `xi_use.hpp`): `is_fault()`, `fault_reason()`,
`fault_key()`, `fault_detail()`, `src()`, `prov()`.
Script build side (`ScriptPackBuilder`, `xi_script_pack.hpp`):
`fault(code, key, detail)`, `src(id)`.
Plugin side (`xi_abi.hpp`): `PackIn::is_fault()/fault_code()` (pre-existing)
`+ fault_key()/fault_detail()/src()/prov()`; `PackOut::fault()` (pre-existing)
`+ src()/prov()` explicit setters.

The discipline mirrors Record NA verbatim:

```cpp
XI_INSPECT_ENTRY(t, frame) {
    auto f = t.pack();
    if (!f) return;                                  // absence: nothing to do
    auto r = xi::use("det0").process(f);             // fault in f? det0 never ran
    if (r.is_fault()) {                              // check BEFORE reading results
        xi::ng(1, ("fault " + std::string(*r.fault_reason()) +
                   " via " + std::string(r.prov().value_or(""))).c_str());
        return;
    }
    // ... real results ...
}
```

## 3. What changed where

| File | Change |
|---|---|
| `backend/include/xi/xi_pack_contract.hpp` | NEW — reserved keys (`kFault/kFaultKey/kFaultDetail/kSrc/kProv/kSeq`), `is_fault`, `prov_parent`/`prov_append`, `propagate_fault`. Speaks only the `xi_pack_v1` vtable, so host / plugin / script share it. |
| `backend/include/xi/xi_abi.hpp` | `pack_contract` constants moved out (namespace name kept); `PackOut::src()/prov()/touched()` (+ stamped flags); `PackIn::fault_key()/fault_detail()/src()/prov()`; `pack_door_abi` stamps `$src`/`$prov` before seal on NON-EMPTY outputs; `emit(PackOut&&)` documented stamp-free. |
| `backend/include/xi/xi_use.hpp` | `ScriptPack::is_fault()/fault_reason()/fault_key()/fault_detail()/src()/prov()`; `process(ScriptPack)` contract comment documents the funnel short-circuit. |
| `backend/include/xi/xi_script_pack.hpp` | `ScriptPackBuilder::fault()/src()`. |
| `backend/src/service_sinks.cpp` | `use_pack_process_cb`: the U1 fault short-circuit (before instance lookup). `use_push_pack_cb`/staging untouched. |
| `plugins/use_pack_door_test.cpp` | Funnel mirror gains the same short-circuit + a door-call counter; sections 7–10: fault round-trip, short-circuit (call count proves the plugin never ran), provenance chain across two chained doors (happy + poisoned), non-fault packs unaffected. |
| `examples/qa_pack_fault_path/` | NEW QA-gated example — the io_stress/fixturing ERROR pattern, pack-only in the live service: script-minted fault → two-hop propagation → plugin-minted contract fault → verdict plane reflects the carried reason/chain. |

Record path: byte-for-byte untouched. Canonical profile: untouched (the new
entries are ordinary canonical str/i64 entries).

## 4. Compatibility

* The stamp's blast radius was MEASURED against the whole suite, and two shape
  contracts pushed back — both honored by design, not exemption: the empty
  door ack stays empty (`touched()` gate; `config_swap_probe_pack_test`
  count==0 green) and emit-path shapes stay producer-exact (`emit()`
  stamp-free; `synced_stereo_test` count==3 and `record_replay_pack_test`
  byte-identity green). Every RESULT-shaped door output is read by key in
  existing consumers, so the two extra entries are additive there. `expose`'s
  wire dump enumerates whatever entries a pack carries — `$src`/`$prov` ride
  to the wire like any reserved key, which is the point.
* A pre-U1 host (no funnel short-circuit) degrades honestly: the fault pack
  enters the door, whose contract check faults on the missing required keys —
  still a fault out, minus the preserved original reason. A pre-U1 plugin
  (old glue) produces unattributed packs — the chain resumes at the next hop.

## 5. Deliberately out of scope (still open after this doc)

* **`_io`-style typed pack build/extract helpers** (the typed-IO third of the
  original U1 bundle; doc 10 codegen gap #2 shipped `_io` Record-shaped only).
  Error-path and provenance no longer wait on it.
* A structured (mp-array) `$prov` with timestamps/verdicts per hop — the flat
  chain is the deliberate v1; revisit only with a consumer that needs it.
* Ordered-sink staging for `use().process` on sink targets (the pre-existing
  v0 gap noted in `use_pack_process_cb`) — orthogonal to fault semantics.
