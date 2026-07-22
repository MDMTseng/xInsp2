# record_replay

The replay **source** (docs/new_gen/10 gate P3) — `record_save`'s mirror. Reads
canonical `.xex1` (XEX1-v3) dumps from a directory and emits each one back into
the graph as a **sealed Pack** through the host `xi.pack@1` door, closing the
loop:

```
record_save  (sink):    sealed pack --shared dump-->  <base>.xex1
record_replay (source): <base>.xex1 --shared parse--> sealed pack -> emit
```

## Fidelity (the P3 parity guarantee)

The file is parsed by `toolbox/expose/src/xex1_pack_parse.hpp` — the same
untrusted-disk edge the host-side loader uses (magic/version gate, ingress
canonicalization, tagged entry walk, tag/value agreement + image-dim sanity
enforced). The reserved `$channel`/`$seq` entries are restored **first** (from
the frame header the sink lifted them into), then every frame entry in wire
order, each rebuilt by its on-wire `XI_PACK_TAG_*`.

Re-dumping the emitted pack through `encode_pack_v3` reproduces the file bytes
**exactly** — record → save → replay is byte-lossless. Proof:
`toolbox/record_replay/tests/record_replay_pack_test.cpp` (drives the real record_save + record_replay
DLLs and asserts original pack ≈ file ≈ replayed pack, byte-for-byte at the
dump level and entry-for-entry at the pack level).

Note on copies: file bytes → pack arena / image pool is one unavoidable ingress
copy (disk bytes are untrusted and pool handles are host-minted); after
`seal()` the pack flows through dispatch zero-copy (handle only).

## Driving model

One `process()` call replays the **next** file (lexicographic order) and
returns a small ack Record — the payload rides the pack plane:

```
{ "replayed":true, "file":"cap_000001.xex1", "channel":"cam0", "seq":7,
  "entries":6, "position":1, "total":10 }
```

A parse failure (truncated/forged/tagless-v2-draft file) emits a sealed
`$fault` pack (fail loud, the pilot convention: code `bad_replay_file`, detail
= the parser's reason) and the ack carries the error; the cursor still
advances, so one bad file cannot wedge a replay loop. Pre-v3 files are refused
by design — see `docs/new_gen/13-replay-file-migration.md`.

## Config

| Key | Default | Meaning |
| --- | --- | --- |
| `dir` | `""` | directory scanned for `*.xex1` (non-recursive) |
| `loop` | `false` | wrap to the first file after the last |
| `enabled` | `true` | gate |

Exchange commands: `{"command":"rewind"}` (rescan + restart),
`{"command":"set_dir"|"set_loop"|"set_enabled", "value":...}`, `get_status`.
`get_def` reflects `position`/`total` for progress display.

record_replay is a **source**: it publishes no pack-in door
(`XI_PLUGIN_PACK_DOOR`); it emits via `new_pack()` + `emit()` exactly like
mock_camera / json_source / synced_stereo in pack mode.
