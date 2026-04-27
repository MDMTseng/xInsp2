# xinsp2 (Python SDK)

Thin synchronous client for the xInsp2 backend WS protocol. Designed to be
driven from a script — including AI agents using a code-execution sandbox
(Claude Code / Claude API). Spec: [`docs/protocol.md`](../../docs/protocol.md).

## Install

```bash
pip install -e tools/xinsp2_py
```

Backend must be running on `ws://127.0.0.1:7823/` (the VS Code extension
auto-starts one; or run `xinsp-backend.exe` from `backend/build/Release/`).

## Quick start

```python
from xinsp2 import Client, dump_run

with Client() as c:
    c.compile_and_load(r"C:\path\to\inspection.cpp")
    c.set_param("sigma", 3.5)
    run = c.run(frame_path=r"C:\path\to\frame.jpg")

    print(f"run {run.run_id} took {run.ms} ms")
    print("count:", run.value("count"))
    gray = run.image("gray")
    print(f"gray: {gray.width}x{gray.height} {gray.codec_name}")

    snap = dump_run(run, "snapshots")
    print("dumped to", snap.folder)
```

## Snapshot format

```
snapshots/run-000017/
    report.json       run metadata + every non-image VAR
    vars/gray.jpg     image previews (jpg/bmp/png by codec)
    vars/edges.jpg
    vars/report.json  one file per `kind:json` VAR
```

Designed so an AI agent can `Read` `report.json` for shape, then read
specific image files only when needed — keeps context small.

## Patterns for AI workflows

- **Compile-fix loop**: catch `ProtocolError` from `compile_and_load`,
  parse the build log out of the message, edit, retry.
- **Param sweep**: loop over `set_param` + `run` + `dump_run`,
  `prefix="sweep_sigma_3p5"` to keep snapshots straight.
- **A/B**: use `c.call("compare_variants", {"a": ..., "b": ...})`
  directly; the SDK doesn't wrap this yet.
- **Long-running observation**: `c.on_log(print)` to mirror backend logs
  to stdout while you drive runs.

## What's NOT in scope

- Multi-client concurrency (single-client v1 backend).
- Async/await — sync is enough for scripted iteration; the read loop
  runs in a daemon thread and the public API is blocking.
- Plugin authoring (that's the C++ SDK at `sdk/`).
