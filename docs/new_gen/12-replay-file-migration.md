# Replay-File Migration — What Happens to Pre-v3 Files (Gate P3 Deliverable)

| Field | Value |
|---|---|
| **Date** | 2026-07-03 |
| **Status** | DECIDED for the polaris2 line; the .json/.bmp conversion question stays OPEN until the app team weighs in (see §4) |
| **Context** | doc 10 gate P3 (persistence parity) — the migration note it requires |

**XEX1-v3** (`.xex1`: magic `XEX1` + canonical max-width msgpack,
`{v:3, channel, seq, frame:{key:[tag, value], …}}`) is the **one durable
record/replay format** from here on. It is written by `record_save`'s pack
door, replayed by `record_replay`, byte-identical to the in-memory pack and
the wire frame (`xex1_v2_identity_test`, `record_replay_pack_test`), and
self-describing (per-entry `XI_PACK_TAG_*` on the wire) — an archive readable
by any stock msgpack decoder without xInsp2.

Four older artifact kinds exist. Per kind, the decision at THE CUT (doc 10's
one coordinated break):

## 1. Tagless "v2 draft" `.xex1` (polaris2 dev runs before 2026-07-03)

**REFUSED by the loader/replayer, permanently** (`unsupported frame version`).

The v2 draft had no per-entry tags, so an image-descriptor map is
indistinguishable from an ordinary nested map of the same shape — a converter
would have to *guess by shape*, which is exactly the defect v3 exists to
remove. We will not launder that guess through a migration tool.

- **What you do:** re-record. These files only ever existed on the polaris2
  dev line (the draft never shipped in any release, never reached the app
  team); losing them costs a re-run, not data.
- **No conversion tooling, by decision** (not by omission).

## 2. Record-era captures: `<base>.json` + `<base>_<key>.bmp` (record_save's legacy door)

**They were never replayable** — no code in xInsp2 has ever read these back
into a run; they are human-readable exports, and they stay exactly that.

- **Until THE CUT:** the Record door keeps writing them, byte-for-byte
  unchanged (bilingual guarantee, gate P1).
- **At THE CUT:** the Record door is deleted with Record itself; existing
  files remain on disk, still human-readable, still not replayable.
- **Offline converter (.json+.bmp → .xex1): FUTURE WORK, not committed.**
  It is mechanically feasible (JSON scalars → tagged entries, BMP → image
  entries) but lossy-in-reverse questions (float formatting round-trip, key
  order, images the JSON never referenced) make it a real project, not a
  script. Decision rule: build it **only if the app team names concrete
  pre-cut captures they must replay**; otherwise the answer is re-record.
  Raise it on the cutover train, not after.

## 3. XEX1-v1 preview frames (the display wire format)

**Not a replay currency at all** — v1 is a *display* frame (values as a JSON
string + JPEG-compressed images, lossy by design). Nothing ever replayed v1
and nothing will.

- v1 stays the **default wire** until the cutover train flips the default to
  v3 (doc 10: retained one release behind a flag, then removed). Decoders
  (webUI, `xex1.py`) stay bilingual v1+v3 until then.
- A v1 frame saved to disk by external tooling keeps decoding with any v1
  decoder for as long as those decoders exist; it was never a record of the
  pack plane and cannot become one.

## 4. In-memory "BufferReplay" (the `cache` plugin)

Unaffected. It re-emits live packs from RAM (hot-param re-inspect), touches no
files, and already went bilingual in gate P1. Listed here only because its
name invites confusion with file replay.

## The support-window statement (honest version)

There is **no read-only support window for pre-v3 files, because there is
nothing to support**: no pre-v3 artifact ever had an in-tree replay path (v2
draft: dev-only + ambiguous; .json/.bmp: export-only; v1: display-only). v3 is
therefore not *breaking* replay compatibility — it is the **first** version
that has any. The only genuine migration cost on the table is §2's optional
converter, and that decision is parked with the app team on the cutover train.
