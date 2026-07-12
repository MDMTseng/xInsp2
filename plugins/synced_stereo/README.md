# synced_stereo

Synthetic stereo camera: a **gathering source** that grabs both cameras and
emits `left` + `right` together under a single trigger. Multi-camera sync needs
no bus policy — the frames are correlated because they ride the same container.
Left is vertical stripes, right is horizontal, both stamped with the same `seq`
in their top-left pixels so a script can verify they came from the same event.

Since THE CUT (v12) the sealed `xi.pack@1` **Pack** is the sole emit currency:
each tick builds ONE sealed Pack carrying the `left` and `right` image entries
plus the `seq` entry, all under a single trigger. Because one emission carries
BOTH images under one trigger, synced_stereo is the showcase for the Pack: a
single sealed container with two image entries *is* the gathering semantic —
no bus correlation policy, just colocation in one immutable container.
(`adopt_image` is a zero-copy addref of the pool slots the frames were painted
into.)

Being a **source**, it has no `process()` input — its script/UI-facing
contract is its **config** (`get_def`/`set_def`), its **commands** (`exchange`),
and the two-image **frame pack** it emits. It follows the plugin data contract
([docs/new_gen/02-plugin-data-contract.md](../../docs/new_gen/02-plugin-data-contract.md))
on those surfaces.

## Keys — one source of truth

Every key name is declared **once** in
[`../../contract/plugins/synced_stereo.decl.json`](../../contract/plugins/synced_stereo.decl.json),
from which `contract/codegen/gen_contract.py` generates `synced_stereo_keys.gen.h`
(the `keys::` constants the plugin compiles against). The derived `has_both()`
is the decl's `"frame_composites"` family and `fire(int n = 1)` its param
`"default"` (the polaris2 codegen-gap-#2 extension).

| Surface | Key | Type | Notes |
|---------|-----|------|-------|
| config  | `fps`       | int  | settable; clamped to [1, 120] |
| config  | `running`   | bool | read-only (`get_def` only) |
| config  | `ticks`     | int  | read-only (`get_def` only) — frames emitted |
| command | `command`   | string | `start` / `stop` / `fire` / `set_fps` |
| command | `value`     | int  | **required** for `set_fps` |
| command | `n`         | int  | frame count for `fire` (headless drive) |
| output  | `left`      | image | vertical stripes |
| output  | `right`     | image | horizontal stripes |
| output  | `seq`       | int  | correlation counter (also stamped into both images) |

Schema version: `xi::synced_stereo::kSchemaVersion` (currently **1**). A
`set_def` carrying a mismatched `$schema` stamp is rejected with a precise
error naming both versions (an absent stamp — a legacy persisted
`instance.json` — is tolerated).

## Using it from a driver / script

```cpp
host.set_def(cam, R"({"fps":30})");
host.exchange(cam, R"({"command":"start"})");
host.exchange(cam, R"({"command":"fire","n":4})");   // headless: emit 4 pairs
// Each dispatched trigger carries ONE sealed Pack; read it through the
// xi.pack@1 accessors (or let expose / a pack door walk it):
//   count() == 3, entries {left, right, seq}, both images correlated by seq.
```

## Tests

The end-to-end proof is the `qa_pack_stereo` example: one `fire` produces
exactly ONE trigger carrying ONE sealed Pack whose entries are exactly
`{left, right, seq}` with both images' pixel-stamped `seq` equal to the pack's
`seq` entry (the gathering invariant). See
`docs/new_gen/12-pack-parity-matrix.md`.
