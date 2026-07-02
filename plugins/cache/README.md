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
| config  | `count`     | int  | current ring occupancy (read-only) |
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

## Tests

`tests/test_cache.cpp` asserts: `replay` with no `index` and `set_capacity` with
no `value` → structured faults; an unknown command falls through to `get_def`;
the `Config`/`Command`/`Capture`/`Status` happy path (capture climbs the buffered
ack, `replay_last` re-emits through a host emit sink, `clear` empties the ring);
and a config schema skew → `set_def` rejects it. Run via
`ctest -C Release -R cache_test` from `plugins/build`.
