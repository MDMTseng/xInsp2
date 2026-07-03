# demo.lut — the resource-handle pattern demo (type-owner lib plugin)

This is a **lib plugin** (docs/new_gen/14) and the executable reference for the
**resource-handle convention** (doc 14 appendix): heavy custom objects do
**not** ride packs — a *type-owner* lib plugin constructs and destructs them,
and packs carry only the **handle entry**, a nested canonical-mp map:

```
{ "type": "demo.lut", "id": <i64 slot>, "gen": <i64 generation>, "$v": 1 }
```

The demo type is an immutable sorted `i64 -> i64` lookup table — the minimal
honest stand-in for any **build-once-query-many** structure (indexes,
calibration tables, model weights). Light objects should NOT use this pattern:
give them a canonical-mp schema and let them ride the pack (sizing doctrine).

## The five rules, as implemented here

1. **All alloc/free inside this DLL.** The ring owns every `Lut`; nothing
   crosses the ABI but handle entries and query answers.
2. **Immutable after construction** (seal semantics): `shared_ptr<const Lut>`;
   mutation = build a new one.
3. **Lifetime = ring/generation lease** (pre-v12): N slots (`ring_slots`
   config, default 8), LRU-recycled under pressure; every recycle bumps the
   slot generation from a DLL-lifetime **monotonic** counter — generations are
   never reused, even across instance reinit, so a stale handle can never
   alias a fresh object. Stale resolve → sealed `$fault "stale_handle"`.
   Owner sweep on crash: the ring dies with the instance; capability
   registrations are owner-swept exactly like imgcodec.
4. **Handle entries are runtime-only.** `demo.lut.dump` is the registered
   **materializer**: a persist sink stores its byte-deterministic canonical
   bin (`XLUT` + u8 ver + be32 count + pairs of be64 key/value) — or drops the
   entry. It never stores the handle.
5. **Wrong-type resolve → `$fault "wrong_type"`.** A handle is only meaningful
   to its owning namespace.

## Capabilities (registry is name-only; `$v`/`$probe` ride in the pack)

| name | in | out |
|---|---|---|
| `demo.lut.build` | mp `keys` (array of i64), mp `values` (same length) | mp `handle`, i64 `built` (0 = content-dedup hit), i64 `builds` (lifetime), i64 `size` |
| `demo.lut.query` | mp `handle`, mp `keys` | mp `values` (i64 per found key, nil per missing), i64 `found`, i64 `size`, i64 `builds` (echo) |
| `demo.lut.dump` | mp `handle` | bin `lut` (deterministic), i64 `size`, i64 `builds` (echo) |

Build is **content-deduped** (FNV-1a over the key/value stream): the same
sealed content builds once; `builds` vs `dedup_hits` is the zero-rebuild proof
counter (`exchange {"command":"stats"}`). `exchange {"command":"recycle_all"}`
is the operator lever: every live slot recycles and all outstanding handles go
stale.

## Proof

- plugins ctest `cap_lut_owner_test` — the real DLL through the real
  capability plane: build/dedup/query, handle hop between two consumers with
  zero rebuild, ring-pressure recycle → `stale_handle`, `recycle_all` →
  `stale_handle`, wrong-type → `wrong_type`, dump byte-determinism, `$probe`/
  `$v`, malformed-handle faults, unregister-on-destroy, pack balance.
- QA `examples/qa_resource_handle` — live service: script → consumer door →
  capability; the handle entry rides packs through a door hop to a second
  consumer; build-counter pinned; stale + wrong-type faults surfaced cleanly.
