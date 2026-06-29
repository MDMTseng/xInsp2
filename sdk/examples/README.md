# SDK example plugins

A library of small, self-contained plugins to read and play with — the Arduino-
`File ▸ Examples` of xInsp2. Each folder is one plugin: a `<name>.cpp`, a
`plugin.json`, and a `CMakeLists.txt`.

## Build one

```sh
cd sdk/examples/<name>
cmake -B build && cmake --build build      # produces <name>.dll
```

Or copy the folder under `<project>/plugins/<name>/` and let the backend compile it
as a project source plugin (then create an instance of it).

## The examples

Start with the basics, then the ones that show off a **special host behaviour**.

| Plugin | What it teaches |
|--------|-----------------|
| [`hello`](hello/) | The smallest possible plugin — read `input.name`, return a greeting. Start here. |
| [`counter`](counter/) | Per-instance state across `process()` calls + a UI showing the running total. |
| [`invert`](invert/) | Image in → image out: read an image by key, allocate, write one back. |
| [`histogram`](histogram/) | Compute over an image (grayscale histogram + mean/stddev/peak). |
| [`trigger_source`](trigger_source/) | A **source**: push one frame per tick into the pipeline via `emit_record`. |
| [`comm`](comm/) | An **ordered output sink** (`"sink": true`): a comm/PLC forwarder that receives records in **frame order** even under parallel dispatch. See below. |

## Special behaviour: the ordered sink (`comm`)

With `parallelism.dispatch_threads > 1`, inspects finish out of order — so a plugin
that forwards results to a PLC would send them out of order. Reordering at the
consumer is fragile (a dropped frame leaves a gap that never fills).

Mark the forwarder's plugin `"sink": true` and the host handles it: a script's
`xi::use("comm0").process(rec)` is **staged** (not run inline mid-inspect) and
**flushed after the inspect, inside the ordered-emit gate**, so deliveries land in
**frame (arrival) order**. Each carries a host-stamped `$seq` (the frame's arrival id)
for correlation; a dropped frame simply never arrives — no gap to wait on. No new
script verb, no new ABI. The global `preview` plugin uses the same mechanism so live
previews stay in frame order too.
