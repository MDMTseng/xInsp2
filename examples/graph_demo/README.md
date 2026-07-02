# graph_demo — seeing a connected pipeline

A minimal project whose only point is to **show data flowing between plugins**
in the Pipeline Graph.

Three instances of `blob_centroid_detector` are chained by their `cleaned`
output image:

```
a ──cleaned──▶ b ──cleaned──▶ c
```

## Try it

1. Open this folder as the workspace in VS Code (it has a `project.json`, so the
   xInsp2 extension activates and the backend auto-compiles the project plugin +
   `inspect.cpp`).
2. Open the **Pipeline Graph** — the type-hierarchy icon in the *Instances* view
   title bar, or run **xInsp2: Open Pipeline Graph**.
3. You'll see the three plugin nodes `a` / `b` / `c`. The script's own per-stage
   compute (counts + provenance) is surfaced through the **`expose` plugin** on
   channel `graph` — read it in the expose panel, not inline in the graph.
4. **Click a node** → opens that instance's webui.
5. Click **⟳ Capture dataflow**. The backend records one inspection and the graph
   draws blue arrows `a → b → c`, each labelled `cleaned` — the observed **image**
   dataflow.

## The two things this demos

- **Click to navigate.** Nodes → webui (tune the plugin). The graph is a map you
  can click through.
- **Provenance (src id).** Every plugin output is stamped with where it came
  from. The script surfaces `a_src` / `b_src` / `c_src` on the expose `graph`
  channel — each equals its instance name (`"a"`, `"b"`, `"c"`). That's the same
  lineage the graph traces
  for images, but carried on the *data* — so the non-image flow is self-
  describing too. (The full extractor→constructor→`$prov` story is in
  [`../fixturing_demo`](../fixturing_demo).)

## Notes

- Capture is **off by default** (zero runtime cost); it only records while you
  press the button.
- Only **image** handoffs are *drawn* as arrows. Scalar/JSON computed in the
  script is surfaced on the expose `graph` channel, not drawn as a faked edge —
  but its `src` is still on the data (see provenance above).
- The capture runs one inspection using the sample frame in `frames/`. A
  source-driven / continuous project is captured live instead.
