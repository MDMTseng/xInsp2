# record_save — example project

**The ack is the receipt.** One sealed pack in, one `.xex1` file out, and what
comes back names the file it just wrote:

```
save seq=0 saved=1 count=1 base=cap_000001.xex1 bytes=2672 psum=118674
```

`count` is the sink's own capture counter — it fills the `{count}` token in
`naming_rule`, so `base` is literally the filename that now exists — and `bytes`
is what was actually written. Nothing advances unless the write succeeded: a full
disk, an IO error or an unsafe `naming_rule` come back `saved=0` with a reason
and the counter stays put. There is no separate "did it work?" channel to poll.

Run it and look in `captures/`:

```
captures/cap_000001.xex1
captures/cap_000002.xex1
...
```

**And `enabled` is a real gate.** Send

```
exchange_instance("rec", {"command":"set_enabled","value":false})
```

and the verdicts change to `saved=0 reason=disabled` while **no** file appears —
the difference between a recorder that is off and a recorder that is broken. The
script maps the two cases apart deliberately:

| ack | verdict | why |
|---|---|---|
| `saved=1` | `xi::ok(1)` | the capture is on disk |
| `reason=disabled` | `xi::result(0)` | nothing to judge — the operator said no |
| any other reason | `xi::ng(1)` | a real failure, surfaced loudly |

**What it shows**

- `$channel` / `$seq` are **reserved**: the sink lifts them out of the entry list
  into the file's frame header (record_replay puts them back). They are how a
  saved capture identifies itself.
- everything else is written with its type tag — scalars, a nested
  canonical-msgpack `meta` entry, and the image. Nested values survive verbatim;
  nothing is flattened.
- the bytes come from the **same encoder** the `expose` plugin pushes on the
  wire, so disk ≈ wire ≈ memory. That is what lets `record_replay` feed the file
  straight back in as a source.
- the sealed capture goes to the sink **and** to the wire — one handle, two
  consumers, zero pixel copies. What you see in the UI is what is in the file.

`output_dir` is resolved relative to the **backend's** working directory, which
is why `instances/rec/instance.json` spells it out from the repo root. `captures/`
is gitignored: it is output, never a fixture.

**Files**: `project.json` (cam + rec + expose), `instances/rec/instance.json`
(the output dir + naming rule), `inspect.cpp`, `driver.py`.

```
python tools/run_qa.py example_record_save
```

The driver asserts *both* halves — every receipt is matched against the real file
(size, XEX1-v3 header, restored `seq`, nested `meta`, and the pixels re-added
against the sealed checksum), and then that a disabled sink writes nothing at
all. Phase 1 on its own would pass just as happily against a sink that ignores
`enabled`.

See also `toolbox/record_replay/example/` (the other end of the loop — it replays
files exactly like these) and `qa/qa_pack_record_replay/` (record → save → replay
asserted byte-for-byte as one graph).
