# lut_owner — example project

A calibration table that never rides a pack.

Your inspection needs a big lookup structure — a calibration curve, a colour
LUT, an index, model weights. Expensive to build, cheap to query, needed on
every frame. Serialising it into a pack per frame is absurd; handing a raw
pointer across the plugin ABI is worse.

`lut_owner` is the executable reference for the third answer, the
**resource-handle convention**: a **lib plugin** owns the objects, and what
crosses the ABI is only a handle entry —

```
{ "type": "demo.lut", "id": <slot>, "gen": <generation>, "$v": 1 }
```

Run it and the driver prints the three claims:

```
steady:   runs=39 stats={'builds': 1, 'queries': 39, 'recycles': 0, 'stale_faults': 0, ...}
recycled: runs=40 stale_reports=1 stats={'builds': 2, 'queries': 79, 'recycles': 1, 'stale_faults': 1, ...}
no owner: runs=29 sample=[(0, 'no demo.lut provider loaded — ungraded, not a defect')]
```

**What it shows**

- **build once, query many.** 39 graded frames, `builds: 1`. The handle
  outlives the tick that made it; that number staying at 1 while `queries`
  climbs *is* the economic argument for the whole pattern.
- **a handle is a lease, not a pointer.** `recycle_all` strands the handle the
  script is holding. It does not dangle and it does not silently alias a new
  object — the generation moved, so it resolves to a sealed `$fault
  "stale_handle"` and the script rebuilds on the spot. One rebuild, then back
  to grading. Losing your lease is normal; code for it.
- **the funnel rc is not the fault.** A stale handle comes back with
  `query_rc == 0`. Transport succeeded; the *answer* was a refusal. Consumers
  that only check the rc will read a stale table as an empty one.
- **the provider is optional.** Remove the `lut` instance and the consumer
  reports `have_cap=0`, the script returns not-applicable (`xi::result(0,…)`)
  rather than a defect, and the line keeps running ungraded.

**Read `plugins/lut_client/src/lut_client.cpp`** — it is the point of this
example. Scripts do not call capabilities, plugins do, and that file is the
entire consumer API in three moves: `get_interface("xi.cap", 1)` once,
`available(name)` before relying on it, `call(name, req, &rsp)` to use it. It
never sees the `lut` instance, its vtable, or the table.

**Try it live**: with this running, send the owner
`exchange_instance("lut", {"command":"recycle_all"})` and watch a `stale=1`
verdict go past, followed by normal ones. `{"command":"stats"}` is the
counters.

**Files**: `project.json` (lut + grader, with the consumer plugin compiled
in-project), `instances/lut/instance.json` (`ring_slots: 4`), `inspect.cpp`,
`plugins/lut_client/`, `driver.py`.

```
python tools/run_qa.py example_lut_owner
```

See also `qa/qa_resource_handle/`, which pushes the same pattern harder: the
handle hops between two independent consumer instances with zero rebuild, and
the wrong-type and byte-determinism (`demo.lut.dump`) legs are asserted too.
