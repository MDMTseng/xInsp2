# 30 — Self-describing blob plane (decision ⑤-final, supersedes the @3 tensor/type_id surface)

Date: 2026-07-14. Decided by CT: **full uniformity** — the image/tensor core
special case retires; every non-scalar payload is a **self-describing blob**.
No legacy-scheme compatibility is owed (the only consumers are a handful of
in-tree plugins/tests; they migrate in the same change). This supersedes
finding ⑤ of doc 28 and closes finding ① structurally (one blob wire arm
forever, instead of a per-type wire tax).

## The one idea

A blob is a pool buffer whose **head describes its own payload**:

```
pool buffer (base is 64B-aligned):
+0   u32  magic   'XBD1' (0x31444258 LE)          — fail-loud discriminator
+4   u32  desc_len                                 — bytes of descriptor msgpack
+8   canonical msgpack map (the descriptor)
+8+desc_len … zero pad …
+payload_off = align_up(8 + desc_len, 64)          — payload, 64B-aligned
+payload_off + payload_len = total buffer length   (payload_len = total - payload_off)
```

- The descriptor is a **canonical msgpack map, string keys** (same canonical
  rules as pack ingress; validated fail-loud wherever foreign bytes arrive).
- Convention key `"t"` — the type string, namespaced by convention
  (`"toolbox-id/type-name"`, e.g. `"xi/image"`, `"acme-scan/profile3d"`).
  Required **by convention** (every typed consumer reads it), but the core seam
  does **not** enforce its presence — `blob_head_validate` checks the descriptor
  is a canonical map and interprets no key (**the core owns no type space**). A
  `"t"`-less blob is well-formed to the core; `type_of` simply returns none.
  Collisions are avoided by namespace, not by a central registry.
- Convention keys for raster/tensor data (used by `xi/image` and friends):
  `"w"`,`"h"`,`"c"` (int), `"dt"` (str: `"u8"|"u16"|"i32"|"f32"|"f64"`).
  Toolboxes add their own keys freely — the core never reads any of them.
  **Sugar boundary (CT, 2026-07-14):** convention-layer sugar stops at 
  `xi/image` (preview/record/examples all consume it). Every other type's 
  payload layout, interpretation, and any internal sub-alignment is the 
  owning toolbox's business, recorded in its own descriptor keys — the SDK 
  grows no per-type helpers for types the framework itself doesn't consume.
- `xi/image` **is just a convention type**: `{"t":"xi/image","w":…,"h":…,"c":…,"dt":"u8"}`.
  There is no image tag, no tensor tag, no dtype enum, no w/h/c in any core
  struct. SDK helpers (mint_image / as_cv / get_image-style accessors) live at
  the convention layer and read the descriptor.

Alignment: base is 64B-aligned (pool guarantee), payload_off is a multiple of
64 → **payload is always 64B-aligned** (SIMD / cv::Mat wrap / future GPU
upload). Cost: ≤ ~64-128B per blob — noise at MB scale, accepted at KB scale.

Zero-copy: unchanged in nature. Producers mint a described buffer FIRST
(`mint_blob(desc, payload_len)` returns the buffer with the head already
written and a pointer to the aligned payload region), fill payload in place
(camera DMA lands at base+payload_off), then adopt into a pack — zero copies.
Foreign bare buffers were always copy-only (adopt has only ever accepted pool
handles); `add_blob(desc, bytes, len)` copies into a freshly minted buffer.

## Core surface after the cut

- `PackTag`: `I64, F64, Bool, Str, Bin, Mp, Blob`. `Image` and `Tensor` are
  **deleted** (not deprecated — deleted; no consumers survive the same change).
- `ExtRecord`: `{handle, total_len}` only — w/h/c/type_id fields deleted.
  A Blob entry is always EXTERN. (Inline Bin/Str/Mp unchanged.)
- `DirEntry::type_id` reverts to reserved-zero; the dtype-in-type_id encoding
  and `kPackTypeUserBase` are deleted.
- C++ `Pack`/`PackBuilder`:
  - `mint_blob(desc) -> BufRef` (descriptor head written, payload region exposed)
  - `adopt_blob(key, BufRef/handle)` — validates the head (magic, desc_len
    bounds, canonical msgpack, payload_off ≤ total)
  - `add_blob(key, desc, payload, len)` — mint + copy convenience
  - `get_blob(key) -> {desc: mp view, payload: span, payload_len}`
  - `type_of(key) -> string_view` (reads `"t"` from the descriptor — sugar)
  - image/tensor-named core verbs are deleted; `xi_cv.hpp` & SDK grow the
    convention helpers instead (`mint_image(w,h,c,dt)` builds the descriptor
    and calls mint_blob; `as_cv`/`as_cv_write` parse it).
- Validation is **one seam**: `blob_head_validate(base,len)` — it lives in the
  lightweight, plugin-safe `backend/include/xi/xi_blob_head.hpp` (depends only on
  `xi_mp.hpp`; `xi_pack.hpp` includes it) so the host container, the C door, AND
  the wire-ingress parser (compiled into the `record_replay` source plugin, which
  must not pull the host pool) all refuse through the SAME code. Used by
  adopt_blob/get_blob, the door, and wire ingress — same fail-loud discipline as
  the add_mp canonicalize seam.

## ABI doors

- **`xi.pack@3` is deleted outright** (it shipped to zero consumers; like the
  never-existed @2, `get_interface("xi.pack",3)` answers NULL forever).
- **`xi.pack@4`** is the blob door, published alongside frozen @1:
  ```c
  typedef struct xi_pack_v4 {
      /* mint a self-describing pool buffer; desc = canonical msgpack map bytes.
         Returns handle (payload writable until adopted+sealed), 0 on invalid desc. */
      xi_image_handle (*blob_mint)(const void* desc, int32_t desc_len, int64_t payload_len, void** payload_out);
      int32_t (*builder_adopt_blob)(xi_pack_builder b, const char* key, xi_image_handle h);
      int32_t (*builder_add_blob)(xi_pack_builder b, const char* key, const void* desc, int32_t desc_len, const void* payload, int64_t payload_len);
      int32_t (*get_blob)(xi_pack_handle f, const char* key, const void** desc, int32_t* desc_len, const void** payload, int64_t* payload_len);
      int32_t (*entry_at)(xi_pack_handle f, int32_t i, xi_pack_entry* out);  /* ordinal walk, tag+key */
  } xi_pack_v4;
  ```
  Layout-guarded from birth (size + last-field static_assert + freeze pins),
  append-only thereafter.
- **`xi.pack@1`**: layout stays frozen (25 slots, guards already in place).
  Scalar/str/bin/mp verbs unchanged. The four image-shaped slots
  (`builder_add_image`, `builder_adopt_image`, `get_image`, and `xi_pack_image`
  consumers) become **door-layer adapters over the blob representation**:
  add_image synthesizes an `xi/image` descriptor + mints + copies/adopts;
  get_image parses the descriptor into `xi_pack_image`. No core special case
  survives — the adapter is ~30 lines at the door, kept because the @1 struct
  layout cannot shrink. In-tree callers still migrate to the SDK helpers; the
  adapter exists so the frozen door never lies.

## Wire (XEX1)

- New **blob arm**: the wire bytes of a Blob entry are the **entire
  self-describing buffer, verbatim** (magic + desc + pad + payload). memory ==
  wire for blobs by construction — the ④A identity extends to the new kind.
- The **image arm is retired**: the encoder never emits it; the parser rejects
  it as unknown-tag (fail-loud), same as any forged tag. Old record files with
  image entries do not replay — accepted (no external consumers; in-tree
  fixtures are regenerated in the same change).
- All three decoder legs (C++ fixtures, Python, JS) gain the blob arm +
  descriptor validation and drop the image arm; golden vectors regenerated;
  FE / preview reads w/h/c/dt from the descriptor of `"t":"xi/image"` blobs.

## What this deletes (contraction ledger)

- `PackTag::Image`, `PackTag::Tensor`, `PackDtype`, dtype-in-type_id,
  `kPackTypeUserBase`, ExtRecord w/h/c/type_id, `xi.pack@3` (whole door),
  finding-⑧'s dtype table (nothing left to bound-check), finding-⑨'s
  adopt_image/get_image guard asymmetry (verbs gone; the ONE adopt path
  validates the head), finding-①'s per-type wire tax (one blob arm forever).
- Doc 28 items reshaped: ⑧⑨ obsolete; ①-full closed structurally; ③
  (sort_idx_ recycle), uint32 seal guard, $channel copy remain and ride along
  with the core package.

## Migration packages

- **A (core)**: xi_pack.hpp re-cut + blob head format + validate seam + SDK/cv
  helpers + core tests. Also folds in: ③ sort_idx_→BuilderScratch, uint32
  seal-size guard, alloc-path memset skip + null hard-reject (zeroinit
  verdict; canvas paths keep zero-fill).
- **B (doors)**: delete @3, add @4 (+ layout guards + freeze pins), @1 image
  slots re-implemented as door adapters; script SDK (ScriptPack) blob verbs.
- **C (wire)**: XEX1 blob arm, image arm retirement, golden regeneration,
  Python/JS/C++ legs, FE preview descriptor read.
- **D (fleet)**: migrate in-tree producers/consumers/tests/examples to the
  SDK helpers; delete dead image-path code.

Sequencing: A first (everything depends on the core types), then B+C+D in
parallel worktrees, then an integration gate on packv3/main.
