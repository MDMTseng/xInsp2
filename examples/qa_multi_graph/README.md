# qa_multi_graph — two independent pipelines in one project/script

Proves the multi-graph reading of the lane/channel topology: **one project,
one script** can host **two fully independent inspection graphs**, each with
its own source, its own analysis instance, its own wire channel, its own
dispatch lane and its own cadence — and a deliberate outage on one graph does
not perturb the other.

```
line A:  camA (mock_camera, pack mode, 64x48 @30fps)          group "line_a"
             └─ trigger ─▶ [GroupLane line_a: queue + 1 worker]
                              └─ script (route: primary_source()=="camA")
                                    └─ detA (blob_analysis door) ─▶ expose ch "a"

line B:  camB (mock_camera, pack mode, 96x72 @10fps)          group "line_b"
             └─ trigger ─▶ [GroupLane line_b: queue + 1 worker]
                              └─ script (route: primary_source()=="camB")
                                    └─ detB (blob_analysis door) ─▶ expose ch "b"
```

## Topology: dispatch groups, not just in-script if/else

The fork is **not** merely a branch inside the script. Each camera's
`instances/<cam>/instance.json` carries a `"group"`; the dispatcher stamps
every emitted trigger with the **emitting instance's** group
(`service_dispatch.cpp`: `ev.group = instance_group(ev.leader_source)`) and
`lane_for_()` routes it to that group's own `GroupLane` (defined in
`service_dispatch.cpp`, held by the engine per `service_internal.hpp`) — its
own bounded queue, its own worker thread(s), its own overflow policy, all
declared in `project.json` → `parallelism.groups`. The script text is shared,
but each *execution* runs on the trigger's own lane; `t.primary_source()`
just picks which graph's hops to run.

So the isolation you get per line is real dispatch isolation:

- own queue: a backlog/drop-storm on one lane never displaces the other
  lane's events (`overflow: drop_oldest` is per-group);
- own worker: a slow/blocked run on line A cannot stall line B's runs;
- own instances: `detA`/`detB` are separate plugin instances (separate state);
- own wire channel: `expose` stores/pushes per channel; `$seq` (stamped from
  each camera's own frame counter) is monotone per channel independently.

## The deliberate fault (U1 pack fault path)

camA seqs `[60, 120)` (a ~2s window) are poisoned in-script: the script mints
a fault pack via `xi::ScriptPackBuilder::fault("frame_timeout", "gray", …)`
and feeds it to `detA`. The host funnel short-circuits — the door never runs,
the reason + `$seq` are carried, `$prov` stamps `detA` — and the verdict is
NG carrying the chain (same semantics as `qa_pack_fault_path`). Channel "a"
keeps flowing with `fault=1` frames, so its wire `$seq` stays monotone
through the outage.

The driver's **count-based isolation proof**: inside the wall-clock span of
line A's NG burst, line B's verdict rate must hold ≥ its nominal 10 fps floor
and ≥ 60% of its own pre-fault rate — plus line B stays all-OK with correct
per-line results (`blob=2, 96x72, prov=detB`) and strictly increasing seqs.

## Where the isolation ends (shared fate — the honest boundary)

Dispatch groups isolate *queues, workers and cadence*. They do **not** make
the two graphs separate deployables. Shared fate remains at:

- **One script.** A script recompile/hot-reload swaps the text under BOTH
  graphs — the reload quiesce pauses line A and line B together. If two teams
  need independent release trains for their pipelines, that's two projects
  (two backends), not two groups.
- **One project lifecycle.** `start`/`stop`/`open_project` are project-wide;
  there is no per-lane start/stop.
- **One process.** A backend crash (or a plugin tearing down the process)
  takes both lines; CPU/memory/image-pool are shared resources — lanes bound
  *scheduling*, not *budgets*.
- **One `expose` sink + one WS wire.** Channels are isolated per key inside
  the sink, but the sink instance and the socket are shared plumbing.

This example demonstrates the first boundary is real (both cameras keep
emitting while a reload would quiesce both lanes) by *documenting* it rather
than hiding it: multi-graph-in-one-project buys lane independence at run
time, and stops at the project/script lifecycle.

## Run

```
python examples/qa_multi_graph/driver.py       # or: python tools/run_qa.py multi_graph
```

Asserts: both lanes exist (`dispatch_stats`); routing purity (every `line_a`
verdict is an "A …" message, every `line_b` a "B …"); line A OK exactly
outside the fault window / NG with `sc=1 reason=frame_timeout prov=detA`
inside; line B all-OK at its own cadence; count-based isolation across the
fault window; per-channel wire `$seq` strictly monotone with channel "a"
spanning the outage.
