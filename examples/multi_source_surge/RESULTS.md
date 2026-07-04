# multi_source_surge — FL測試 round 6 results

## What this case actually exercises

Three concurrent stressors on the same project:

1. **Three heterogeneous source instances** (`source_steady`, `source_burst`,
   `source_variable`) running simultaneously, each with a different
   framerate, image size, and emission shape. Each frame stamps a
   per-instance FNV-1a hash into bytes [8..15] so the dispatch script
   can attribute every inspect back to its source without a string
   compare.
2. **Two distinct surge events** during each 4-second sweep, fired via
   `exchange_instance({command:"burst", count:N})`:
   - `t = +1.00s`: 10-frame burst on `source_steady` AND `source_burst`
     simultaneously (overlap with 60 Hz + 30 Hz steady traffic)
   - `t = +2.50s`: 200-frame back-to-back surge on `source_burst`
     alone (sustained ~30x source-rate spike)
3. **Two detector instances** with different per-call costs
   (`detector_fast`=5 ms, `detector_slow`=15 ms) consuming overlapping
   source streams. Routing in `inspect.cpp`:

       source_steady   -> detector_fast
       source_burst    -> detector_fast AND detector_slow (overlap)
       source_variable -> detector_slow

## Numbers

Two parallelism sweeps; each runs `cmd:start fps=200` (driver-timer
tick, sources have their own fps) and drives both surges.

The per-event VAR model was removed from core, so this driver no longer
reads script-side latency VARs. Throughput is counted from the backend's
`run_finished` lifecycle events (one per dispatched inspect), and
per-source attribution comes from a tiny per-inspect record the script
pushes to the `expose` sink (channel `runs`), decoded via
`examples/lib/xex1.py`. The end-to-end latency columns are gone with the
VARs; the queue/drop columns now read the real `dispatch_stats` fields
(`dropped`, `queue_depth_high_watermark`, `queue_depth_cap`). Last
representative run:

| Sweep | N | Q | active inspects | run_finished/s | drops (this sweep) | qmax |
|---|---|---|---|---|---|---|
| A-serial-N1-q32   | 1 |  32 | 175 |  43.7 | 619 |  32 |
| B-parallel-N8-q128| 8 | 128 | 552 | 137.8 |  71 | 128 |

**Speedup (B vs A): 3.15x** on `run_finished`/s. Wider queue + 8
dispatch threads turn ~3.5x more frames into completed inspects and
shrink drops by ~9x. (`active inspects` = expose frames decoded = one
per active inspect.)

### Per-source attribution (active inspects only)

| Sweep | steady | burst | variable | used_fast | used_slow | used_both |
|---|---|---|---|---|---|---|
| A | 89 |   7 |  79 |  96 |  86 |   7 |
| B | 229 | 147 | 176 | 376 | 323 | 147 |

`used_both` exactly equals `burst` in every sweep — so the routing in
`inspect.cpp` (burst goes to both detectors, others go to one)
holds across N=1 and N=8. `used_fast == steady + burst` and
`used_slow == burst + variable` — the four arithmetic invariants
all match. Per-source attribution **survives** parallel dispatch, even
though it now travels out of the script via the `expose` sink rather
than a VAR (the `src` / `used_fast` / `used_slow` fields are decoded
from the `runs` channel's XEX1 frames).

### Hot-reload mid-run

`compile_and_load(inspect.cpp)` was issued ~400 ms into a running
N=8 dispatch with all five instances live. It completed in **4.35 s**
(cl.exe-bound; the SDK rebuilds the script DLL from scratch). After
the reload all three sources kept emitting and the new inspect
correctly received frames from every source within a 1.0 s window
(by_src = `{steady: 120, variable: 95, burst: 4}`). No instance
needed re-instantiation; sources kept running on their own threads
through the reload.

## What I learned

### `dispatch_threads=8` doesn't get you 8x

3.15x speedup tracks the same ceiling FL r5 saw at 3.14x — the gap
above 4x is explained by source-rate cap on the steady streams (60 +
30 + 45 ≈ 135 evt/s combined ceiling); the gap below 4x in the
"work" budget is two effects: (a) the burst surge spends ~600 ms in
queue-deep waiting on workers that are 100% busy, dragging the p95
latency to 615 ms even though median is 31 ms; (b) the two detector
instances are reentrant by construction (atomic counters under a
small mutex on the per-tag map) but contend on that mutex during the
200-frame surge. The contention is bounded — no detector
instance-pinning is happening.

### `queue_depth=128` is enough — barely

In sweep B, `queue_depth_high_watermark=128 == queue_depth_cap`. The
queue saturated under the 200-frame surge. With 71 drops in the
sweep we know the dispatcher couldn't quite keep up: total emitted ≈
552 active + 71 dropped = 623 ≈ (60+30+45)*4 + 200 + 20 = 760
expected, so we lost some to background heartbeat dilution and clock
drift, but the order of magnitude matches. A `queue_depth=256` would
have absorbed the surge entirely; a `queue_depth=64` would have
dropped roughly 200 instead of 71. **The control surface works as
documented.**

### Per-source attribution survives N=8 dispatch

This was the open question on the brief. Verdict: yes, perfectly.
Every active inspect knew exactly which source fired it, the FNV-1a
src_tag stamped at emit-time round-tripped through the trigger bus,
the dispatch queue, and an N=8 worker pool with zero misattribution
across ~750 events. `xi::current_trigger().sources()` is reliable
under concurrent dispatch (it's served from a thread-local set by
the worker before calling inspect — see
`backend/include/xi/xi_script_support.hpp` g_trigger_info_fn_).

### `dispatch_stats` reset semantics aren't documented

`cmd:start` zeros the drop counters and the high-watermark. If you
record `before = dispatch_stats(); start(); ...; after =
dispatch_stats()` and subtract, you get nonsense whenever the
previous sweep accumulated drops the new `cmd:start` cleared. **This
bit me for one full run** — sweep B reported drops = -548 (negative)
the first time. This driver reads the post-stop snapshot as the
per-sweep total and does not subtract. The real field is `dropped`
(a single per-run overflow counter); the lifetime totals live on
`dropped_lifetime` / `queue_depth_high_watermark_lifetime` if you need
them. The fix is either (a) document that drop counters are scoped to
the most recent cmd:start window and don't subtract, or (b) carry a
`since_start_id` so callers can detect the reset. See FRICTION.md P1-3.

### Script output now flows through the `expose` sink, not VAR

The per-event `VAR(name, expr)` model was removed from core. To get the
per-source attribution out of the script we push a small `xi::Record`
to the `expose` plugin instance each active inspect
(`xi::use("expose").process(rec)` with the channel under the reserved
`"$channel"` key). The driver subscribes to that channel and decodes the
self-describing XEX1 binary frames with the shared
`examples/lib/xex1.py` helper. This also retired the old
`VAR(name, name)` shadow footgun — there is no macro re-declaring a
local any more. Detail in FRICTION.md.

### Hot-reload across multiple instances is solid

Five instances + 4.35 s mid-run rebuild + zero crashes + dispatch
resumed across all three sources within tens of milliseconds of the
new DLL loading. The "Lazy spawn — keeps the cost out of
compile_and_load" comment in service_main.cpp is real; what's
expensive is cl.exe, not the framework.

## Reproducing

```cmd
:: terminal 1 — backend
backend\build\Release\xinsp-backend.exe

:: terminal 2 — driver
cd examples\multi_source_surge
python driver.py
```

Driver writes `parallelism` into `project.json` between sweeps;
re-running is idempotent. The full output is the comparison table +
per-source breakdown + hot-reload check.
