# xinsp2 (Python SDK)

Thin synchronous client for the xInsp2 backend WS protocol. Designed to be
driven from a script — including AI agents using a code-execution sandbox
(Claude Code / Claude API). Spec: [`docs/protocol.md`](../../docs/protocol.md).

## Install

```bash
pip install -e tools/xinsp2_py
```

Backend must be running on `ws://127.0.0.1:7823/`. Two ways to get one:

- Inside VS Code, the xInsp2 extension auto-starts one when you open a
  project — nothing to do.
- Outside VS Code (CI, scripts, agent runs), launch it yourself in the
  background: `backend/build/Release/xinsp-backend.exe &`. The SDK
  doesn't start it for you. If you see `ConnectionRefusedError`, that's
  what's missing.

## Output: the `expose` model

Scripts surface output through the **`expose`** plugin (the VAR replacement):
a `xi::Record` of scalar values + images is pushed to a named **channel**
(string). Each record is delivered to clients as ONE atomic binary frame —
magic `XEX1` + a minimal msgpack body `{v, channel, seq, json, images[]}`.
The SDK decodes it into an `ExposeFrame{channel, seq, values, images}` where
`images` maps each key to its JPEG bytes.

Subscription is **per channel**, tracked inside the plugin over its
`exchange` (not a backend WS command). Subscribe first, then each `run()`
pushes a frame for every subscribed channel; you can also pull the latest on
demand with `get_expose`.

## Quick start

```python
from xinsp2 import Client, dump_run

with Client() as c:
    c.compile_and_load(r"C:\path\to\inspection.cpp")
    c.set_param("sigma", 3.5)

    c.subscribe(["lane"])                       # subscribe channels you want pushed
    run = c.run(frame_path=r"C:\path\to\frame.jpg")

    print(f"run {run.run_id} took {run.ms} ms")
    frame = run.expose("lane")                  # ExposeFrame | None
    if frame:
        print("values:", frame.values)          # the scalar dict (count, score, ...)
        jpeg = run.image("lane", "gray")         # bytes | None
        print("gray jpeg bytes:", len(jpeg or b""))

    snap = dump_run(run, "snapshots")
    print("dumped to", snap.folder)

    # Pull the latest for a channel without subscribing:
    latest = c.get_expose("lane")               # ExposeFrame | None
    # Channel metadata (seen counts, subscription state):
    meta = c.list_channels()
```

## Snapshot format

```
snapshots/run-000017/
    report.json            run metadata + per-channel values + image manifest
    lane/values.json       the channel's scalar values dict
    lane/gray.jpg          each exposed image (always JPEG)
    lane/edges.jpg
```

Designed so an AI agent can `Read` `report.json` for shape, then read
specific image files only when needed — keeps context small.

## Patterns for AI workflows

- **Compile-fix loop**: catch `ProtocolError` from `compile_and_load`,
  parse the build log out of the message, edit, retry.
- **Param sweep**: loop over `set_param` + `run` + `dump_run`,
  `prefix="sweep_sigma_3p5"` to keep snapshots straight.
- **A/B**: drive the two variants yourself — `set_param` + `run` +
  `dump_run` with a distinct `prefix` per variant, then diff the snapshots.
- **Long-running observation**: `c.on_log(print)` to mirror backend logs
  to stdout while you drive runs.

## Capturing the VS Code UI

```python
from xinsp2 import screenshot
p = screenshot()             # %TEMP%/xinsp2_screenshot_<ms>.png
# or screenshot("./shot.png")
```

Wraps a PowerShell call to `System.Drawing.Graphics.CopyFromScreen`
(Windows only — same approach the e2e suites use). Captures the
entire primary display; assumes VS Code is visible.

## What's NOT in scope

- Multi-client concurrency (single-client v1 backend).
- Async/await — sync is enough for scripted iteration; the read loop
  runs in a daemon thread and the public API is blocking.
- Plugin authoring (that's the C++ SDK at `sdk/`).
