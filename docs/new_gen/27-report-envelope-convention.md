# 27 — Report-Envelope Convention (`pack["report"]`)

Status: **officially recommended convention** (polaris2). Not ABI-enforced — a
producer that ignores it still emits a valid pack; but official tooling (the
VSCode extension, `expose`, `record_save`, dashboards) reads this shape to show
per-instance status/results **without knowing the producer's schema**. Follow it
and the tools understand your plugin for free; ignore it and they fall back to
guessing.

Companion to doc 15 (pack fault semantics) and doc 07 (uniform keyed-buffer
plane). Nothing here changes the pack ABI: `report` is ONE ordinary nested-
msgpack entry (`XI_PACK_TAG_MP`) among the pack's top-level entries.

---

## 1. The layout

A conforming output pack has three planes at the TOP level:

```
pack:
  img0, img1, …          # images — TOP-LEVEL, zero-copy pool buffers (NEVER nested)
  $fault, $src, $prov,   # host reserved plane — UNCHANGED (doc 15)
  $seq, $channel, …
  report                 # ONE nested msgpack map: { status, data }
```

`report` is a canonical-msgpack map (doc 07 profile: string keys, max-width
tags) with exactly two recommended keys:

```
report = {
  "status": {
    "state":      str,   # "ok" | "ng" | "na" | "fault"   (run-outcome vocabulary)
    "value":      int,   # sub-code: ok-n / ng-n / fault code; 0 when not applicable
    "msg":        str,   # human detail / short log line; "" when none
    "elapsed_us": int    # wall time this process() took; omit or -1 if unmeasured
  },
  "data": {
    …producer result keys…   # the actual process output (scalars / str / nested mp)
  }
}
```

`state` reuses the per-run verdict class vocabulary
(`outcome_class_for_code`, `service_result.cpp`) so one word means the same
thing everywhere. Per-PACK state is one of **`ok` / `ng` / `na` / `fault`**.
(`not_executed` / `not_invoked` are HOST per-frame aggregate states — an
instance that never ran emits no pack, so they never appear inside a `report`.)

`value` is the numeric channel split out from the old signed-code convention:
the class lives in `state` (flat, never sub-categorised), the index/measure lives
in `value`. So `ok2` is `{state:"ok", value:2}` — no more packing category and
index into one signed int, no reserved-band squatting.

---

## 2. Normative rules

1. **`report` is recommended, not required.** Producers SHOULD emit it. Tools
   MAY assume it but MUST degrade gracefully when it is absent (fall back to the
   top-level `$fault` marker for state, and generic top-level enumeration —
   `PackIn::for_each` — for data).

2. **`$fault` STAYS top-level; `report.status` only MIRRORS it.** The host
   funnel short-circuits and propagates faults on the **top-level** `$fault`
   entry (`PackIn::is_fault`, `pack_contract::propagate_fault`, doc 15). A
   producer signalling a fault MUST stamp top-level `$fault` (+ `$fault_key` /
   `$fault_detail`); it MAY additionally set `report.status.state="fault"` (with
   the same reason in `msg`) for tooling. **`report.status` is never the poison
   signal** — a fault that lives only inside `report` will not short-circuit
   downstream and is a bug.

3. **Images never go inside `report`.** They are zero-copy pool buffers; nesting
   them into msgpack would force a copy. Images stay top-level.

4. **`data` holds producer payload only** — no `$`-reserved keys, no images.

5. **Canonical msgpack** (doc 07): string keys, max-width profile; key order is
   the producer's contract order.

---

## 3. What tooling reads (the point of the convention)

Given `report`, an official tool renders a plugin it has never seen:

| Tool surface | Reads |
|---|---|
| status badge (red/green/grey) | `report.status.state` |
| tooltip / verdict line | `report.status.msg`, `report.status.value` |
| per-instance timing | `report.status.elapsed_us` |
| result inspector (values) | `report.data.*` (generic key/value walk) |

Absent `report`, the same tool falls back to: top-level `$fault` → state,
generic top-level enumeration → data. So a non-conforming plugin still shows
*something*; a conforming one shows the right thing.

---

## 4. Producer / consumer ergonomics (helper — landed)

Hand-building the nested map through `xi::mp::Writer` is possible but verbose
(canonical msgpack needs the map element count up front). The helper pair makes
the convention the easy path — `xi_pack_report.hpp` (`ReportOut` / `ReportIn`)
over the count-free msgpack builders in `xi_mp_build.hpp` (`MapBuilder` /
`ArrayBuilder` / `Doc`, plus `MapReader` / `ArrayReader`), which backpatch the
map/array count at close via three additive `xi::mp::Writer` primitives
(`open_map` / `open_array` / `set_count`). Builders nest maps and arrays to any
depth in write order (LIFO; a debug-only `dbg_leaf_` guard asserts the rule with
zero release cost):

```cpp
// WRITE (plugin side, over PackOut)
auto rep = out.report();                    // buffers entries
rep.status().ok().value(1).elapsed(us);     // or .ng(code) / .fault(code,key,detail)
rep.data().i64("count", 5).f64("score", .97);
rep.finish();                               // emits mp("report",{status,data});
                                            // a fault ALSO stamps top-level $fault (rule 2)
out.image("img0", w, h, c, px);             // images stay top-level

// READ (consumer / tool side, over PackIn)
auto rep = in.report();
if (rep.status().is_fault()) { … }          // == top-level in.is_fault()
auto count = rep.data().i64("count");       // std::optional<int64_t>
auto us    = rep.status().elapsed_us();

// or path access with a typed fallback (scalars) / a reader (containers):
int64_t x    = rep.get<int64_t>(".data.objects[3].x", -1);
auto    objs = rep.getNode(".data.objects").asArray();   // == rep.get<ArrayReader>(".data.objects")
```

The `fault()` writer stamps BOTH planes so rule 2 holds by construction — a
producer using the helper cannot accidentally hide a fault inside `report`.

Reads are **width-tolerant / downward-compatible**: `MapReader`/`ArrayReader`
share coercions (`as_i64`/`as_f64`/`as_bool`) so `i64()` accepts any integer
width (uint8…int64), `f64()` accepts any integer or float, and `boolean()`
accepts a real bool or an integer 0/1 — a consumer never has to match the exact
wire type. `MapReader` (keyed) and `ArrayReader` (indexed) expose the same typed
getter set, mirroring the writers (`add(key,…)` / `push(…)`).

For tooling that renders a report it has never seen, the helpers also carry:
`Node::kind()` + `is_map()/is_array()/is_int()/…` (type introspection),
`MapReader::for_each(fn(key, Node))` (generic key walk), `ArrayReader::
to_vector<T>()` / `for_each_node()` (typed element iteration), `bytes()`/`data()`
on every view + `MapBuilder::add_raw`/`add(key, reader)` (splice a subtree
verbatim, no re-encoding), and `xi::report::{kOk,kNg,kNa,kFault, State,
state_from, state_str}` + `StatusView::state_enum()` so the `state` vocabulary
is a shared enum, not a re-typed string at each site.

---

## 5. Migration note

This is a data-LAYOUT convention, not an ABI change, but producers and
consumers of a given pack must adopt it together: a reader that switches to
`report.data.count` breaks against a producer still writing top-level `count`,
and vice-versa. Roll it per data-seam (producer + its consumers), newest
pipelines first; existing flat plugins keep working until migrated.
