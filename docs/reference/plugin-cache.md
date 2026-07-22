# `cache` plugin — frame ring buffer + replay

The `cache` plugin (class `BufferReplay`, `toolbox/cache/src/cache.cpp`) is the
plugin-side **capture/replay store**. It buffers the last *N* incoming sealed
packs (images + metadata entries) in a bounded ring and re-emits a buffered one
on command, so a script can re-inspect a frame it already saw **without the
source re-grabbing**.

This is the HDevelop-style **hot-param re-inspect loop**: buffer a frame, tune a
`Param`, replay to re-run `inspect()` on the *same* frame with the new value.

Since capture/replay is a plugin (not a host facility), the ring lives entirely
behind the `xi.pack@1` door + `exchange()` surface — **zero core involvement**.

## Wiring

Add an instance of plugin `cache` and feed it live frames from the inspect
script via `xi::use(...)`:

```cpp
// inspect.cpp
auto t = xi::current_trigger();
const std::string src = t.primary_source();          // live source, or "buffer" on replay
if (src != "buffer")                                 // don't re-buffer a replayed frame
    xi::use("buffer").process(t.pack());             // capture the current sealed pack
```

The `src != "buffer"` guard is required: a replayed pack is emitted with the
cache instance's own name as its source, so without the guard every replay would
re-buffer itself and the ring would fill with duplicates.

Instance config (`instances/<name>/instance.json` or `project.json`):

```json
{ "plugin": "cache", "isolation": "in_process", "config": { "capacity": 16 } }
```

`capacity` (int, default 16, 1..1024) bounds the ring; the oldest entry is
evicted once it is exceeded.

## Contract

### `process(pack) -> {"buffered": <ring size>}`
RETAINS the incoming sealed pack into the ring (zero pixel copy — a retained
sealed pack keeps itself and its pool image handles alive beyond the frame that
produced it), evicting (= releasing) the oldest beyond `capacity`.

### `exchange({...}) -> get_def()`
Every command returns the post-mutation `get_def()`:
`{"capacity": N, "count": <ring size>}`.

| Command | Effect |
|---|---|
| `{"command":"replay_last","n":k}` | re-emit the last `k` buffered records (default 1, clamped to ring size), oldest-first, **back-to-back** |
| `{"command":"replay_all"}` | re-emit every buffered record, oldest-first, back-to-back |
| `{"command":"replay","index":i}` | re-emit the record at ring index `i` |
| `{"command":"replay_timed","speed":s,"n":k}` | **TIMED replay** — re-emit the buffered records (all, or the last `k`) on a background thread, paced by the ORIGINAL inter-capture gaps, scaled by `speed` (>1 faster, default 1.0). Returns immediately; `get_def().replaying` is `true` until it finishes |
| `{"command":"stop_replay"}` | cancel an in-flight timed replay |
| `{"command":"clear"}` | cancel any timed replay and empty the ring |
| `{"command":"set_capacity","value":v}` | resize the ring (evicts oldest if it shrinks) |

`get_def()` reports `{"capacity", "count", "replaying"}`; poll `replaying` to
detect when a `replay_timed` run has drained.

### Timed vs back-to-back
`replay_all`/`replay_last` fire as fast as dispatch accepts them — good for the
hot-param re-inspect loop (you want the answer immediately). `replay_timed`
reproduces the **arrival cadence** (each capture's gap, optionally time-scaled) —
good for exercising load/timing behaviour (queue pressure, overflow) against a
recorded traffic shape. Neither reproduces byte-identical ordering; that is
on-disk deterministic replay, a separate feature.

Re-emission re-emits the **same sealed pack handle** (zero copy) through the
pack emit door, with the cache instance's name as the source and a **fresh**
trigger id + `ts = now` (a replay is a new dispatch event, not a byte-copy of
the original arrival — see the record-replay design notes if you need
id/timestamp fidelity, which is a separate feature).

## Reference example

`examples/buffer_replay_demo/` is the runnable reference:
- `inspect.cpp` — captures each live frame into instance `buffer`, computes a
  `thresh`-dependent `over` count, and publishes results to the `expose` sink on
  channel `"runs"`.
- `driver.py` — end-to-end regression: fires more frames than `capacity` and
  asserts the ring bounds; then replays the **same** buffered frame under two
  `thresh` values and asserts `over` tracks the threshold — proving the replay
  re-runs `inspect()` on the buffered frame with no re-grab. Runs its own backend
  on a private port (so it is unaffected by a running VS Code extension holding
  the default `7823`).

Run it:

```
cd examples/buffer_replay_demo
PYTHONPATH=../../tools/xinsp2_py:../lib python driver.py
```

Expected: `RESULT: PASS` (ring bound + hot-param re-inspect + no-re-buffer + clear).

## Scope

The `cache` plugin is an **in-memory** ring — it does not persist to disk and
does not preserve original trigger ids / arrival timestamps on replay. For
deterministic on-disk capture and byte-identical replay (a journal + a
`replay_source`), that is a separate, larger feature tracked in the record-replay
design notes; `cache` covers the practical "buffer the last N frames and
re-inspect" need.
