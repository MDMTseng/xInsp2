# expose — example project

The script's only data-out surface, and the thing people get wrong about it:
**subscription gates the wire, not the record.**

A `mock_camera` frame comes in. Every run pushes **two** channels:

| channel   | payload                                   | size    |
|-----------|-------------------------------------------|---------|
| `measure` | `seq`, `mean`, `min`, `max`, `bright_pct` | ~200 B  |
| `detail`  | the frame **and** a threshold mask        | JPEGs   |

Open the webUI and you get a tab per channel. Watch `measure` tick while
`detail` sits idle; click the `detail` tab and the images start flowing. Drag
the `threshold` Param and the mask repaints under your hand.

**What it shows**

- `xi::use("expose").push(pack)` is the entire API. No VAR table, no image
  register, no side channel — a sealed pack with a `"$channel"` key on it.
- **channels are how you separate cheap from expensive.** A script doesn't push
  "the output"; it pushes a numbers lane you leave streaming forever and a
  pixels lane nobody looks at most of the time.
- **an unwatched channel costs a store and nothing else.** expose keeps the
  latest record per channel unconditionally, but JPEG-encodes and broadcasts
  only channels a client has `subscribe`d to. That is why a fat debug channel
  can stay in a production script.
- **nothing is lost by not subscribing.** `list_channels` reports every channel
  ever written, with its `seen` count and `subscribed` flag; `get {channel}`
  base64s that channel's latest frame on demand. Push/pull, not push/nothing.
- **one record, many images.** `frame` and `mask` ride the same pack, so they
  can never be displayed a frame apart.
- the verdict plane (`xi::ok`) and the data plane (expose) are separate. One
  says pass/fail, the other says what you look at; neither substitutes.

```
python tools/run_qa.py example_expose
```

The driver asserts the gate in **both** directions, which is the only way to
assert it at all:

```
A  subscribed=[measure]         measure=36  detail=0
A  list_channels -> {'detail': {'seen': 37, 'subscribed': False}, ...}
A  get(detail)   -> images=['frame','mask']
B  subscribed=[measure,detail]  measure=36  detail=36
C  subscribed=[measure]         measure=23  detail=0
```

Phase A is the load-bearing one: zero `detail` frames on the socket while
`seen` climbs to 37 and `get` still hands the whole record over. A check that
only confirmed "the subscribed channel arrived" would pass just as happily
against a backend that broadcast everything.

**Files**: `project.json` (cam + expose), `instances/`, `inspect.cpp`,
`driver.py`.

See also `qa/qa_pack_stream/` (expose carrying a structural summary for a test
to read) and `qa/qa_jpeg_preview/` (the encode path itself).
