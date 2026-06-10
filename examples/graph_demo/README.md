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
3. You'll see the three plugin nodes `a` / `b` / `c` with the script's `VAR`
   chips interleaved (those are the script's own compute, in source order).
4. Click **⟳ Capture dataflow**. The backend records one inspection and the graph
   draws blue arrows `a → b → c`, each labelled `cleaned` — the observed **image**
   dataflow. Click any node to open its webui.

## Notes

- Capture is **off by default** (zero runtime cost); it only records while you
  press the button.
- Only **image** handoffs are drawn. If a stage pulled a number/JSON out of a
  Record, computed on it, and fed the result onward, that provenance is lost in
  the script's C++ — it shows as a `VAR` chip, not a faked edge.
- The capture runs one inspection using the sample frame in `frames/`. A
  source-driven / continuous project is captured live instead.
