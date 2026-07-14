# 28 — packv3 slab-pack review findings (2026-07-14)

Two review rounds on the packv3 slab-pack line, before merging `packv3/main`
back toward the mainline. Method: 9 background review agents across two rounds
(round 1 = static/unit angles; round 2 = dynamic/system + a charter-level
philosophy challenge). Findings are file:line-anchored against
`packv3/hardening` (commit `aed5de7` code + `3406a15` docs, on `packv3/main`).

**Line state at review time**
- `packv3/main` = slab container (`d8fe140`) + identity/docs (`1c16ca5`) +
  the `xi.pack@3` door (`c0b4b73`), merged `b5d3468`.
- `packv3/hardening` = `+aed5de7` (the three A-items below) `+3406a15` (their
  docs). Not yet merged to `packv3/main`.

## Status legend

- **DONE** — landed on `packv3/hardening`.
- **DEFER-AUTONOMOUS** — cheap, charter-aligned, no ABI/wire semantics change;
  safe to batch and land without an owner decision.
- **DECIDE-CT** — a direction call reserved for CT (breaks a frozen surface,
  contradicts a design-of-record, or trades a stated deciding property).

---

## Already landed (batch A — `aed5de7` / `3406a15`)

- **A1 — TLS teardown safety.** `SlabPool` + builder `BuilderScratch` freelists
  moved behind trivially-destructible thread_local pointers (pixpool-magazine
  doctrine, `xi_image_pool.hpp` detail::tls_magazine). A `Pack` outliving its
  producer thread and dropped in late thread_local/static teardown now frees
  its slab outright instead of touching a destroyed freelist vector.
  `xi_pack.hpp` `tls_slab_pool`/`slab_acquire`/`slab_release`,
  `tls_scratch_pool`.
- **A2 — pack-door layout tripwires.** `xi_pack_v1`/`v3` get a `static_assert`
  (size + last-field offset) in `xi_abi.h` plus per-field `XI_FREEZE_IFACE`
  pins in `test_abi_freeze.cpp`. These vtables resolve by version through
  `get_interface`, so the `xi_host_api` size guard never covered them; a
  mid-struct verb insert would have silently rewired every compiled pack
  plugin. Now a build failure.
- **A3 — spent-builder guards.** `spent_()` on every `PackBuilder` mutator and
  `seal()`: a sealed/moved-from builder refuses **structurally** (empty Pack /
  no-op) instead of dereferencing null `s_` in a Release build where the assert
  compiles out.

---

## Round 2 — the high-severity findings (dynamic / system / philosophy)

### ① @3 Tensor/blob cannot round-trip on the wire — silently. **fail-loud violation.**
Convergence: integration F1–F4 × philosophy #2/#4 × round-1 core #5. Confidence: HIGH.

- Tensor entries hit `default: continue` in every dump walk —
  `plugins/expose/src/xex1_pack_dump.hpp:99` (shared by expose pull/store AND
  `record_save`), `xex1_wire_preview.hpp:138`, `expose.cpp:358`; the host walk
  agrees (`xi_pack.hpp` `canonical_value` returns false for `PackTag::Tensor`).
- The XEX1-v3 format cannot carry a tensor: `xex1::V3Entry`
  (`xex1_encode.hpp:147`) has no tensor arm; the parser tag/value switch
  (`xex1_pack_parse.hpp:237-271`) has **no `case XI_PACK_TAG_TENSOR`** → tag 7
  → `agree=false` → the **whole frame** is refused as forged/corrupt.
- User-typed blobs lose `type_id`: every dump path reads via plain `in.bin` and
  re-emits anonymous `bin32` (`xex1_pack_dump.hpp:79-85`,
  `xex1_wire_preview.hpp:99-105`, host `canonical_value` Bin arm); rebuild calls
  plain `add_bin` (`xex1_pack_load.hpp:76-80`, `record_replay.cpp:122-126`), so
  it returns with `type_id = 0`.
- **Observable:** `add_tensor`/`add_blob` → emit through expose or persist via
  `record_save` → tensor silently gone / blob type flattened. `record → save →
  replay` is **not** byte-lossless for these entries. Contradicts the v3 banner
  ("recovers every entry's type EXACTLY", `xex1_encode.hpp:176-179`) and the
  frozen ABI tag set is a *superset* of what the wire can represent (F4).
- **Root-cause amplifier (F3):** the ONLY callers of
  `add_tensor`/`add_blob`/`get_tensor`/`get_blob`/`type_id_of` are
  `backend/tests/*` — **no production plugin, script, or example** drives @3.
  The identity round-trip test never builds a tensor/blob, so it structurally
  cannot catch this. A door nobody drives is untested integration.
- **Fix options:**
  - **DEFER-AUTONOMOUS (stopgap):** make the dump walks + `canonical_value`
    **fail loud** (mint a `$fault` / hard error) on a Tensor / user-blob entry
    instead of silently dropping — honours fail-loud until the loop is closed.
  - **DECIDE-CT (full):** close the wire loop — a `V3Entry` tensor descriptor
    `{w,h,c,dtype,elems:bin}` + a blob `[tag,type_id,value]` shape, parser cases
    with dim×elem_size checks, rebuild via `builder_add_tensor`/`_blob`; add an
    end-to-end fixture (build tensor+blob → encode → parse → assert entry+type
    equality) as the acceptance test.

### ② Concurrency: creator-tag over-release → UAF; `reinit()` missing OwnerGuard is the trigger.
Convergence: concurrency F1+F2. Confidence: MED-HIGH.

- The single-creator-tag invariant (`xi_pack_abi.hpp:57-66`) rests on an
  **unenforced** precondition: the creator's seal ref must be retired under
  `OwnerGuard(creator)`. `release_as` clears the tag only on owner match
  (`xi_pack_abi.hpp:163-165`). If a creator drops its own seal ref from a
  context where `current_owner()==0` (a self-spawned background/emit thread; the
  bus path is `release_as(pack, 0)`), `creator_ref_live` stays `true` and lies —
  it still authorises the sweep to drop one more ref.
- **Interleaving:** A seals P (rc1, creator=X, live) → consumer Q retains
  (rc2) → A drops its own ref off-guard (owner 0 ≠ X, tag stays live, rc2→1,
  the survivor is Q's) → X torn down → `release_all_for(X)` sees `creator==X &&
  live` → rc1→0 → P destroyed while Q reads it → **UAF / silently freed pack.**
- **Concrete trigger:** `reinit()` destroys the old instance **without** an
  `OwnerGuard` (`xi_cabi_adapter.hpp:621-624`); the adapter dtor path does wrap
  it (`:314-315`). `reinit()` runs on a dispatch/inspect worker not under
  `OwnerGuard(owner_id_)`, so any pack ref the old instance releases in its dtor
  hits `release_as(pack, 0)` → the stale-tag path above.
- **Good news (F4):** the pack path **inherits** the `cmd_remove_instance_`
  quiescence fix — remove/rename/close/recompile/rebuild/commit + process-exit
  all quiesce lanes before a pack sweep (`service_cmd_project.cpp:309/339/363`,
  `service_dispatch.cpp:578-636/729-770`). `reinit()` is the ONE
  adapter-mutating path that runs without lane quiesce (relies on
  `cap_gate_`), which is exactly why its unguarded destroy matters.
- **Fix (DEFER-AUTONOMOUS):** wrap `destroy_fn_(old)` at `xi_cabi_adapter.hpp:623`
  in `ImagePool::OwnerGuard g(owner_id_)`, matching the dtor. Stronger: make the
  creator ref an actual counted ref instead of a boolean the last non-matching
  release can strand (turns the convention into structure).

### ③ `sort_idx_` heap-allocates on every `seal()` — contradicts the "heap-free after warmup" claim.
Convergence: perf F1. Confidence: HIGH.

- `std::vector<uint32_t> sort_idx_` is a **member of `PackBuilder`, not of the
  recycled `BuilderScratch`** (`xi_pack.hpp`, PackBuilder private). A builder is
  built fresh per pack and spent after `seal()`, so the "reused capacity"
  comment is false in real usage: `seal()`'s `sort_idx_.resize(n)` is one
  `malloc` + one `free` per pack, per frame. The whole SlabPool/scratch
  apparatus exists to make steady-state builds heap-free; this punches one
  alloc/free straight through it.
- **Fix (DEFER-AUTONOMOUS):** move `sort_idx_` into `BuilderScratch` (recycled
  with `payload`/`entries`), or SBO for `n ≤ ~32`. Same file, same discipline
  as A1.

---

## Round 2 — the charter-level challenges (DECIDE-CT)

### ④ `memory ≠ wire` is a net-negative trade against the design's own deciding property.
Philosophy #1. Charter values at stake: 5 (慣例→結構), 1 (速度優先), 6 (收縮).

- Doc 07 sold `memory == canonical-msgpack` as a **deciding property**:
  "boundaries become copies, not transformations — expose stops re-encoding,
  replay stops re-parsing" (`07-uniform-keyed-buffer-plane.md:60-64`). The slab
  threw it away: scalars are raw in memory, wire bytes are produced on demand at
  every serialization boundary by `canonical_value`. Verified consumers
  re-encode: `xex1_encode.hpp:139-143` ("`raw_at` is NOT wire bytes anymore").
  expose + record_save are the generic sinks walking every entry, so the
  boundary doc 07 promised as a memcpy is now a per-entry re-encode, forever.
- What was bought: a raw-scalar read the design **itself** rates as noise on a
  KB plane next to megapixel buffers (`07:99,172-174`). No bench isolates the
  trade (the cited 522ns number measured the since-deleted TypedPack vs a JSON
  DOM). Identity was retired on a number that measures something else.
- **Key insight:** the raw-scalar choice is **separable** from the slab. A slab
  storing canonical-msgpack inline keeps every structural win (hash-sorted dir,
  O(log n), one recycled block, insertion-order table, EXTERN handles) **and**
  restores `memory == wire` — `canonical_value` collapses to `raw_at`, the
  identity is structural not test-enforced. This is 慣例→結構 run backwards: a
  convention the walker must keep honest, instead of a structure that can't be
  got wrong.
- **Alternatives:** (A, smallest diff) inline payloads = canonical msgpack
  again, splice at the boundary; (B) full slab-verbatim wire per the prototype
  `serialize()` (`proto/xi_pack_c.hpp:1201-1235`) — a *different* memory==wire,
  explicitly deferred by `xi_pack.hpp` ("NOT this migration"). Landed state has
  *neither* identity.

### ⑤ The user-blob type space puts a domain type ladder into core.
Philosophy #2 × integration × security. Charter values: 2 (精緻極小核心), 3 (plugin).

- `kPackTypeUserBase = 0x100` + `add_blob(type_id)` + `get_blob`/`type_id`
  across the @3 door is "the caller's typed-blob space for future custom
  payloads" — with **zero current consumers**. This contradicts doc 07 D2,
  which prescribes typed/dimensioned things as **plugin conventions over
  Bin+Mp**, not core types ("the core owns buffers and dispatch, not images",
  `07:33-36,113-121`).
- **Concession:** `PackTag::Tensor` (= Image with u8 generalised to a dtype) is
  a small honest core extension — keep it; `adopt_bin` fixes a real wart
  (producers masqueraded byte buffers as fake-dim images for zero-copy adoption)
  — keep it.
- **Contraction:** delete `add_blob` + `kPackTypeUserBase` (+ the @3
  `add_blob`/`get_blob` type_id space). Put the producer's type tag inside the
  msgpack descriptor, read by the accessor layer — the doc-07-D2 prescription.
  DECIDE-CT because it removes verbs from the (not-yet-consumed) frozen @3
  surface.

### ⑥ Contract now: get `proto/xi_pack_c.hpp` out of `include/`.
Philosophy #3 × security #4. Charter value: 6 (膨脹再收縮).

- 1288 lines of non-production code carrying a **divergent** design (hash-order
  iteration that loses insertion order, 5 dtype ids, slab-verbatim serialize),
  living in `include/` where a reader can mistake it for a second real pack. Its
  value was already harvested (the pixpool size-class recycler was backported).
- Its `deserialize()` (`proto/xi_pack_c.hpp:1239`) is a genuine per-entry
  **unbounded OOB deserializer** (validates header only; `DirEntry`
  `off/len/key_off` unchecked) — harmless today (test/bench only) but dangerous
  if ever promoted.
- **Contraction:** lift the bench numbers + design-C rationale into a doc,
  delete the header from `include/` (or move under a non-shipping `bench/`
  tree). DECIDE-CT only because it's CT-authored experiment record (value 6
  carve-out: "don't reclaim what CT deliberately kept").

---

## Round 2 — defense-in-depth (DEFER-AUTONOMOUS, latent today)

- **⑦ `add_mp` ingress bypass** (security #1, the only foreign-reachable one).
  "Ingress is the only path" is a **convention, not structure**: `f_add_mp`
  (`xi_pack_abi.hpp:273-276`) and `PackOut::mp` (`xi_abi.hpp:221-224`) bypass
  canonicalize; only `ScriptPack::add_mp` and `ingress::canonicalize_into` gate.
  Example plugins call the raw ABI directly. Hostile foreign msgpack (incl.
  handle-shaped ext) can enter a pack entry and travel onto the wire /
  self-DoS a replay file. **Fix:** canonicalize at the C-ABI seam (or in
  `PackBuilder::add_mp` itself), dropping the "trusts" contract.
- **⑧ dtype OOB + storage confusion** (security #2/#3 × round-1 core #3).
  `pack_dtype_elem_size` does unchecked `k[uint16_t(d)]` (5 elems); `get_tensor`
  casts `PackDtype(type_id)` with no range check; inline readers don't verify
  `storage==Inline`. Gated off from every foreign channel today (no
  foreign→slab route; @3 trampolines range-check dtype 0..4), so
  trusted-code-robustness only. **Fix:** bound-check the table, reject
  out-of-range dtype in `add_tensor`/`get_tensor`, assert `storage==Inline` in
  inline readers.
- **⑨ `adopt_image`/`get_image` missing size + F1 guards** (concurrency F3 ×
  security #5 × round-1 core #9 — **triple convergence**). `adopt_image`
  addrefs + bumps `ext_live_` without the `view()` liveness/size check its
  `adopt_tensor`/`adopt_bin` siblings have; `get_image` has no null/short-view
  F1 guard, so a dead-handle image surfaces nonzero dims with an empty span (a
  caller trusting `w*h*c` over-reads). **Fix:** give `adopt_image` the sibling
  size guard; add the F1 guard to `get_image`.
- **⑩ perf misc.** `n<=1` seal early-out; `bump_` `resize()` zero-fills then
  memcpy overwrites (double-write, notable only for large inline Str/Bin/Mp);
  keep a linear-scan `find` branch for small packs (the old container's
  `kLinearMax=24` optimisation the slab dropped — pending measurement); write
  `DirEntry`/`PackHeader` fields straight into the slab instead of `d{}`+memcpy.

---

## Round 1 — findings (static / unit angles), condensed

Ranked; those escalated or superseded by round 2 are cross-referenced.

- **uint32 offset/length truncation** unguarded — `seal()` `payload_off` and
  `bump_` are uint32 while `slab_bytes` is uint64; large bins go EXTERN but
  inline `add_mp` has **no** `kPackLargeThreshold`, so a huge nested Mp can push
  offsets past 4 GiB and silently truncate. Add a `slab_bytes <= UINT32_MAX`
  assert/fault at seal. (round-1 core #7) — DEFER-AUTONOMOUS.
- **`propagate_fault` drops `$channel`** — copies `$fault*`/`$seq`/`$stream`/
  `$part`/`$eof` but not `$channel` (`xi_pack_contract.hpp:148-183`), so a
  short-circuited fault loses its routing lane. Decide intent + comment or copy
  it. (round-1 SDK #8) — DEFER-AUTONOMOUS (small).
- **pre-@3 host: silent truncation** — on a pre-@3 host `add_tensor`/`add_blob`
  return false and the sealed pack simply **lacks** the entry (not a `$fault`).
  A producer using the v1 fluent style and ignoring the bool ships a truncated
  pack with no poison marker. Add SDK doc + debug warn-once. (round-1 SDK #7).
- **Test coverage gaps** (round-1 tests): duplicate-key / hash-collision path
  (the memcmp run + first-inserted-wins) **untested**; cross-thread slab free
  **untested** (headline lifecycle claim, all tests single-thread);
  tensor/blob/image exhaustion fallbacks untested (only large-bin); dtype
  matrix 2/5 (F64/I32/U8 unexercised); empty-pack / empty-key / degenerate
  inputs untested; the identity double-encode shares `xi::mp::Writer` on both
  sides so it is airtight only as a **whole suite** (identity + `test_mp_fixtures`
  + `test_canonical_xcheck`) — add one hand-written literal anchor.
- **perf baseline is a silent SKIP** — `perf_baselines/pack.txt` carries no
  fingerprint header, so `perf_gate.cmake` SKIPs-with-reason and no regression
  can be caught; recapture is prose, not a tracked task. Surface it loudly (a
  visible "BASELINE MISSING" warning) or file the recapture. (round-1 tests + perf).
- **Doc drift not gated** (round-1 SDK): the retired `memory ≈ wire` /
  `arena_bytes` tokens are still taught in live headers (`xi_mp.hpp:187`,
  `xex1_encode.hpp:26/230`) and are **not** in `tools/check_retired_terms.py`,
  so the drift passes the gate — violates the "retired concepts进 fail-loud 守衛"
  rule. (Note: partially superseded by ④'s direction call — if memory==wire is
  restored, this reverses.)

---

## Suggested sequencing

1. **DEFER-AUTONOMOUS batch B** (no ABI/wire semantics change): ② reinit
   OwnerGuard, ③ sort_idx_ recycle, ⑨ adopt_image/get_image guards, ⑧ dtype/
   storage bound-checks, ⑦ add_mp seam canonicalize, ①-stopgap (fail-loud on
   tensor/blob dump), plus the round-1 offset/`$channel`/perf-gate-visibility
   items. All "convention → structure / fail-loud".
2. **DECIDE-CT** — resolve ④ (memory==wire restore), ⑤ (delete blob type
   space), ⑥ (proto out of include/), and ①-full (close the wire loop) before
   `packv3/main` merges to the mainline. ④/⑤ are cheapest to change **now**,
   before any external consumer freezes the surface.

Nothing here blocks the A-batch (`aed5de7`/`3406a15`) — those were the
independent, safe closures and are done.
