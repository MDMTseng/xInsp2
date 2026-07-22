# SDK example plugins

A library of small, self-contained plugins to read and play with — the Arduino-
`File ▸ Examples` of xInsp2. Each folder is one plugin: a `<name>.cpp`, a
`plugin.json`, and a `CMakeLists.txt`.

Every example speaks the **xi.pack@1 data plane** — since the v12 ABI cut it is
a plugin's sole data path: a worker overrides
`process(xi::PackIn&, xi::PackOut&)` and publishes the door with
`XI_PLUGIN_PACK_DOOR(Class)`; a source builds packs with `new_pack()` and hands
them to the host with `emit()`. (The old `xi::Record` process path is gone.)

## Build one

```sh
cd sdk/examples/<name>
cmake -B build && cmake --build build      # produces <name>.dll
```

Or copy the folder under `<project>/plugins/<name>/` and let the backend compile it
as a project source plugin (then create an instance of it).

To poke a built example without a backend, use the headless host-mock CLI:

```sh
sdk/host_mock/xi_run_plugin.exe hello/hello.dll --meta "{\"name\":\"you\"}"
sdk/host_mock/xi_run_plugin.exe invert/invert.dll --image src=photo.png
```

## The examples

Start with the basics, then the ones that show off a **special host behaviour**.

| Plugin | What it teaches |
|--------|-----------------|
| [`hello`](hello/) | The smallest possible plugin — read str `"name"` from the input pack, answer a greeting. Start here. |
| [`counter`](counter/) | Per-instance state across door calls + a UI showing the running total (and a pack-door DLL test under `tests/`). |
| [`invert`](invert/) | Image in → image out: read an image entry by key, `pool_image()` a fresh output, `adopt_image()` it into the pack zero-copy. |
| [`histogram`](histogram/) | Compute over an image (grayscale histogram + mean/stddev/peak) — scalars as typed entries, the counts[256] array as one nested msgpack entry (`xi::mp::Writer`). |
| [`trigger_source`](trigger_source/) | A **source**: push one frame per tick into the pipeline via `new_pack()`/`emit()` on an SEH-safe `xi::spawn_worker` thread. |
| [`comm`](comm/) | An **ordered output sink** (`"sink": true`): a comm/PLC forwarder whose door receives pushed packs in **frame order** even under parallel dispatch. See below. |

## Special behaviour: the ordered sink (`comm`)

With `parallelism.dispatch_threads > 1`, inspects finish out of order — so a plugin
that forwards results to a PLC would send them out of order. Reordering at the
consumer is fragile (a dropped frame leaves a gap that never fills).

Mark the forwarder's plugin `"sink": true` and the host handles it: the sink feed is
a script's `xi::use("comm0").push(pack)` — it is **staged** (not run inline
mid-inspect) and **flushed after the inspect, inside the ordered-emit gate**, so
deliveries to the sink's pack door land in **frame (arrival) order**. Push is
fire-and-forget (the door's reply pack is dropped); request-reply
`process(pack)` on a declared sink is refused at the funnel, since a staged call's
reply cannot exist mid-inspect. Each delivery carries the reserved i64 entry `$seq`
(the frame's arrival id) for correlation; a dropped frame simply never arrives — no
gap to wait on. No new script verb, no new ABI. The global `preview` plugin uses the
same mechanism so live previews stay in frame order too.
