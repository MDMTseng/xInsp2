# cache (BufferReplay)

Replay as plugin composition: the plugin-side record/replay store. `process(in)`
captures the incoming record into a bounded ring; an exchange command re-emits a
buffered record via `xi::emit_record` so the script re-runs on it — the
HDevelop-style hot-param loop (buffer a frame, tune a Param, `replay_last` to
re-inspect the SAME frame with the new value, no camera re-grab).

## Open payload, typed control surface

cache replays **whatever** record it captured — it is producer-agnostic by
design, so the buffered payload is an **open schema** (docs/new_gen/02, like
`record_save` / `expose`). There is deliberately no typed view over the buffered
data.

What *is* a fixed, declarable vocabulary is its rich **command** surface, its ring
**config**, and the small `process()` **capture-ack**. Those follow the plugin
data contract:

## Keys — one source of truth

Every control key is named **once** in [`src/cache_keys.h`](./src/cache_keys.h);
the plugin's own readers and the typed view ([`src/cache_io.h`](./src/cache_io.h))
compile from it.

| Surface | Key | Type | Notes |
|---------|-----|------|-------|
| config  | `capacity`  | int  | ring size (settable) |
| config  | `count`     | int  | current ring occupancy — records + packs (read-only) |
| config  | `packs`     | int  | of which are retained sealed packs (read-only) |
| config  | `replaying` | bool | a timed replay is in flight (read-only) |
| command | `command`   | string | `replay_last` / `replay_all` / `replay` / `replay_timed` / `stop_replay` / `clear` / `set_capacity` |
| command | `n`         | int  | `replay_last` count / `replay_timed` limit |
| command | `index`     | int  | **required** for `replay` |
| command | `speed`     | double | `replay_timed` pacing scale |
| command | `value`     | int  | **required** for `set_capacity` |
| output  | `buffered`  | int  | ring size after a `process()` capture |

Schema version: `xi::cache::kSchemaVersion` (currently **1**). A config built
against a different version is rejected by `set_def`; an absent stamp (legacy
persisted def / the plugin.json default) is tolerated.

## Failure shape

A recognized command missing a required payload fails loud rather than silently
doing nothing:

```json
{ "error": "missing_input", "key": "index", "expected_type": "int" }
```

Unknown commands still fall through to `get_def()` — the driver "status" idiom
the `examples/buffer_replay_demo` relies on.

## Using it from a driver / script

```cpp
#include "cache_io.h"

host.set_def(buf, xi::cache::Config().capacity(32));
// hot-param re-inspect: replay the last buffered frame under a new Param
host.exchange(buf, xi::cache::Command::replay_last(1));
host.exchange(buf, xi::cache::Command::replay_timed(2.0));   // paced x2, background

auto st = xi::cache::Status{ host.get_def(buf) };
// process() ack, when a script drives capture directly:
xi::cache::Capture cap{ xi::use("buffer").process(rec) };   // cap.buffered()
```

## Pack retention (polaris2 wave-2 — bilingual)

cache speaks **both** currencies through **one ring**. Alongside the Record path
above it publishes the `xi.pack@1` pack-in/pack-out door
([`XI_PLUGIN_PACK_DOOR`](./src/cache.cpp)), and a ring `Entry` is a variant: a
deep-copied **Record**, or a **retained reference to a sealed host Pack**.

- **Capture** (`process(PackIn&, PackOut&)`): the door `retain`s the incoming
  sealed pack into the ring and acks `{buffered: N}` — the same ack shape the
  Record path returns. Retaining a sealed pack keeps it **and its pool image
  handles** alive *beyond the frame that produced it*: this is the whole point
  of a buffer/replay store on the pack plane.
- **Replay** re-emits the **same sealed pack handle** through the host emit door
  (`emit_pack`) with a **fresh trigger id** — **zero pixel copy**. The replay
  commands are ring-agnostic (they act on whichever entry kind sits at the
  target slot); `replay_timed` paces **one loop** over the variant entry, so
  records and packs interleave and replay in capture order.
- **Eviction / `clear` / `set_capacity` shrink / teardown** all `release` every
  retained pack — dropping the ring's owning ref, which frees the sealed pack
  and its pool buffers when no live consumer remains.

**Zero-copy is proven, not asserted** (`tests/test_cache_pack.cpp`): the
replayed event carries the *identical* handle the ring holds, and
`ImagePool::cumulative().live_now` is **unchanged** across a replay.

### Retention safety & the one registry finding

A retained pack outliving its producer is safe by construction:
`ImagePool::release_all_for` (the producer-destroy owner sweep) drops **exactly
one ref per entry** rather than force-freeing — an image a cache pack still holds
(refcount > 1) survives, orphaned to the anonymous owner and freed by its last
holder. That contract was written **naming buffer_replay** as the caching
consumer it protects, and the static-teardown `g_image_pool_alive` guard means
even a pack destroyed after the pool is gone releases safely. So there is **no
ordering hazard** at project close *provided the plugin releases its own refs* —
which the destructor does.

The asymmetry this finding flagged is now **closed** (pack-plane hardening):
the PackRegistry keeps an owner-tagged ref ledger and
`PackRegistry::release_all_for` is the exact analogue of the ImagePool's
sweep. A pack-retaining plugin that forgets to release on destroy has its
outstanding refs reclaimed by the adapter dtor / script unload, with a
"swept N leaked pack ref(s)" diagnostic — mirroring leaked images. Releasing
on teardown is still good manners (a swept ref is a reported bug, not a
feature); cache discharges it in `~BufferReplay`, so its sweep count is 0.
The regression test for the original scenario lives in
`backend/tests/test_pack_door.cpp` (owner-sweep section).

## Tests

`tests/test_cache.cpp` asserts: `replay` with no `index` and `set_capacity` with
no `value` → structured faults; an unknown command falls through to `get_def`;
the `Config`/`Command`/`Capture`/`Status` happy path (capture climbs the buffered
ack, `replay_last` re-emits through a host emit sink, `clear` empties the ring);
and a config schema skew → `set_def` rejects it.

`tests/test_cache_pack.cpp` covers the **pack** side against the real DLL: capture
N packs → ring stats + the `PackRegistry` / `ImagePool` oracles climb by N;
eviction and `clear` release; **zero-copy** replay (same sealed handle, fresh id,
stable pool live-count, identical image bytes); a mixed records+packs ring
replays in order; and destroy releases everything back to baseline.

Run both via `ctest -C Release -R cache` from `plugins/build`.
