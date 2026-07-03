# synced_stereo

Synthetic stereo camera: a **gathering source** that grabs both cameras and
emits `left` + `right` together under a single trigger. Multi-camera sync needs
no bus policy — the frames are correlated because they ride the same container.
Left is vertical stripes, right is horizontal, both stamped with the same `seq`
in their top-left pixels so a script can verify they came from the same event.

It is **bilingual** (polaris2 wave-2, the Pack migration — see
[docs/new_gen/10-pack-migration-scope.md](../../docs/new_gen/10-pack-migration-scope.md)):
by default it emits the two-image **Record** exactly as it always has, and the
opt-in `pack_mode` config gathers the pair into ONE sealed `xi.pack@1` **Pack**
per trigger instead. Because one emission carries BOTH images under one trigger,
synced_stereo is the showcase for the Pack: a single sealed container with two
image entries *is* the gathering semantic. See [Pack mode](#pack-mode) below.

Being a **source**, it has no `process()` input Record — its script/UI-facing
contract is its **config** (`get_def`/`set_def`), its **commands** (`exchange`),
and the two-image **frame** it emits. It follows the plugin data contract
([docs/new_gen/02-plugin-data-contract.md](../../docs/new_gen/02-plugin-data-contract.md))
on those surfaces: a typed builder for config/commands and a typed extractor for
the emitted stereo frame.

## Keys — one source of truth

Every key name is declared **once** in
[`../../contract/plugins/synced_stereo.decl.json`](../../contract/plugins/synced_stereo.decl.json),
from which `contract/codegen/gen_contract.py` generates `synced_stereo_keys.gen.h`
(the `keys::` constants) and `synced_stereo_schema.gen.h` (the left+right+seq
frame slots) and `synced_stereo_io.gen.h` (the typed `Config`/`Command`/`Frame`
view). The derived `has_both()` is the decl's `"frame_composites"` family and
`fire(int n = 1)` its param `"default"` (the polaris2 codegen-gap-#2 extension),
so this plugin is now a **full swap**: the hand-written `_io.h` is deleted and
every consumer includes the generated header (see `contract/codegen/README.md`,
"Coverage").

| Surface | Key | Type | Notes |
|---------|-----|------|-------|
| config  | `fps`       | int  | settable; clamped to [1, 120] |
| config  | `pack_mode` | bool | opt-in (default **false**) — emit one sealed `xi.pack@1` Pack instead of a Record |
| config  | `running`   | bool | read-only (`get_def` only) |
| config  | `ticks`     | int  | read-only (`get_def` only) — frames emitted |
| command | `command`   | string | `start` / `stop` / `fire` / `set_fps` |
| command | `value`     | int  | **required** for `set_fps` |
| command | `n`         | int  | frame count for `fire` (headless drive) |
| output  | `left`      | image | vertical stripes |
| output  | `right`     | image | horizontal stripes |
| output  | `seq`       | int  | correlation counter — pack-mode canonical entry (also stamped into both images) |

Schema version: `xi::synced_stereo::kSchemaVersion` (currently **1**). The
`Config` builder stamps it so a header/plugin skew reports precisely.

> **Adoption note.** `synced_stereo.cpp` now `#include`s `synced_stereo_keys.gen.h`
> and its **pack path** compiles against the generated `keys::kLeft` / `kRight` /
> `kSeq` / `kPackMode` constants. The **Record path keeps the string literals
> `"left"` / `"right"` byte-for-byte** (the wave-2 rule: the default path is
> untouched), and `tests/test_synced_stereo.cpp` **pins those literals** by
> reading a really-emitted Record through the extractor — so any drift between
> the plugin's Record literals and this header turns the test red.

## Using it from a driver / script

Include the generated `synced_stereo_io.gen.h`:

```cpp
#include "synced_stereo_io.gen.h"

host.set_def(cam, xi::synced_stereo::Config().fps(30));
host.exchange(cam, xi::synced_stereo::Command::start());
host.exchange(cam, xi::synced_stereo::Command::fire(4));   // headless: emit 4 pairs

// Read an emitted pair with the typed extractor — both images live in one
// record, which IS the correlation guarantee.
xi::synced_stereo::Frame f{ emitted_record };
if (f.has_both()) correlate(f.left(), f.right());
```

## Pack mode

Set `pack_mode: true` (default is `false`) and the emit currency flips: each tick
builds ONE sealed `xi.pack@1` Pack carrying the `left` and `right` image entries
(pool-backed — `adopt_image` is a zero-copy addref of the *same* pool slots the
Record path would hand over, so the pixels are identical) plus the `seq` entry,
all under a single trigger. Everything else — the two distinct stripe patterns,
the `seq` stamped into each image's top-left pixels, the fps/start/stop/fire
control surface — is unchanged. A host that does not publish the `xi.pack@1`
plane degrades safely back to the Record path.

```cpp
// Opt in (no typed setter — the hand-written Config only exposes fps today):
host.set_def(cam, R"({"pack_mode":true})");
host.exchange(cam, xi::synced_stereo::Command::fire(1));
// The dispatched trigger now carries ONE sealed Pack; read it through the
// xi.pack@1 accessors (or let expose / a pack door walk it):
//   count() == 3, entries {left, right, seq}, both images correlated by seq.
```

This is the migration showcase: the gathering source's "both images, one
trigger" semantic is preserved *exactly* by a single sealed Pack with two image
entries — no bus correlation policy, just colocation in one immutable container.

## Tests

`tests/test_synced_stereo.cpp` asserts: the `Config`/`Command` builder happy path
(fps set + clamped through `get_def`); that with pack mode OFF `fire` emits one
**Record** carrying BOTH images, read end-to-end through the typed `Frame`
extractor; and that with pack mode ON one `fire` produces exactly ONE trigger
carrying ONE sealed **Pack** whose entries are exactly `{left, right, seq}` with
both images correlated by that `seq` (the gathering invariant), while pooled-
handle balance holds across `fire` and `start`/`stop`. Run via
`ctest -C Release -R synced_stereo_test` from `plugins/build`.
