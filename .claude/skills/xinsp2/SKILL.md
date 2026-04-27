---
name: xinsp2
description: A Python client (`tools/xinsp2_py`) that wraps the xInsp2 backend WS protocol — compile a C++ inspection script, run it on a frame, read out VARs and image previews, dump runs to disk. Available when working on inspection scripts / plugins / projects in this repo. Use it, ignore it, or bypass it — whichever fits.
---

# xinsp2 — what's available

This is a **map**, not a recipe. Pick what helps; skip what doesn't.

## What exists

- **Python SDK** at `tools/xinsp2_py/` — sync `Client` over WebSocket. Wraps a few common commands (`compile_and_load`, `set_param`, `run`); everything else goes through `c.call(name, args)`. Source is small (~200 lines), read it if you want to know exactly what it does.
- **`dump_run(run, dir)`** — writes a run to disk as `report.json` + decoded image files. Lets you `Read` artifacts later instead of holding bytes in memory. Optional.
- **Raw protocol** in `docs/protocol.md`. The SDK is a convenience layer; if it gets in the way, talk to the WS directly.

## When the SDK is probably worth it

- You'll run multiple iterations and want to diff outcomes
- You need actual image output to verify, not just code-level reasoning
- A param sweep / A-B / regression check across frames

## When to skip it

- Pure code review / design discussion
- Single-shot "does this compile" — just edit and let the user hit run
- Anything where the user is already driving the UI

## Connection

Default URL `ws://127.0.0.1:7823/`. Backend is auto-started by the VS Code extension; if not running, `backend/build/Release/xinsp-backend.exe &` works. Install: `pip install -e tools/xinsp2_py`.

## Shape of a run

```python
from xinsp2 import Client
with Client() as c:
    c.compile_and_load(r"<abs path to .cpp>")
    run = c.run(frame_path=r"<abs path>")
    # run.vars              -> list of {name, kind, value/gid, ...}
    # run.value("count")    -> scalar VAR
    # run.image("gray")     -> PreviewFrame (gid, codec, w, h, payload)
    # run.previews[gid]     -> raw access
```

`ProtocolError` from `compile_and_load` / `recompile_project_plugin`
carries only the *short* error string (e.g. `"compile failed"`). The
**full build log arrives separately as a `log` event**. To capture it,
register a handler before compiling:

```python
logs = []
c.on_log(lambda m: logs.append(m))
try:
    c.compile_and_load(path)
except ProtocolError:
    print("\n".join(m["msg"] for m in logs if m["level"] == "error"))
```

Or read the on-disk log: `%TEMP%\xinsp2\script_build\inspect_v*.log`.

If you see mojibake in the log on a Chinese-locale Windows, set
`VSLANG=1033` in the environment that started the backend — `cl.exe`
otherwise emits CP-950 messages.

## Project + live-tune flow (when you're working on a real project)

Driving a project (with `project.json` + instances + project-local
plugins) is two cmds, not one:

```python
c.load_project(r"<abs path to project folder OR project.json>")  # reattach instances
c.compile_and_load(r"<abs path to inspect.cpp>")                  # recompile script
run = c.run()
```

`load_project` does **not** auto-compile the inspection script. Cold
opens with N project-local plugins compile every plugin under cl.exe;
the SDK passes a 180 s timeout for these calls, but the first run of a
fresh project can still be slow.

To hot-reload one project plugin after editing its source (the live-
tune loop):

```python
c.recompile_project_plugin("local_contrast_detector")
# instances of that plugin are re-attached with their previous defs;
# next c.run() uses the new code.
```

## Things that worked before (not prescriptions)

- **Param sweep**: loop `set_param` + `run` + `dump_run(prefix=…)`, tabulate scalar VARs, only open images for the interesting points.
- **A/B**: `c.call("compare_variants", {"a": {...}, "b": {...}})` does both runs server-side and returns both snapshots.
- **Compile-fix**: catch `ProtocolError`, edit the .cpp based on the build log, retry.
- **Live log mirror**: `c.on_log(print)` while iterating.

These are patterns observed to work, not the only way.

## Context vs disk

`dump_run` exists because image bytes in conversation context are expensive. After dumping, `Read snapshots/run-NNNNNN/report.json` for shape, then read specific image files only when needed. If you're doing 1-2 runs and want everything in working memory, skip `dump_run` — use `run.image(...)` directly.

## Discovering plugins

`cmd:list_plugins` and `cmd:open_project` reply with each plugin's
metadata. If the plugin's `plugin.json` includes an optional
`"manifest"` block (params / inputs / outputs / exchange — see
`docs/reference/plugin-abi.md`), it lands as `plugin["manifest"]` in
the reply. Plugins without one still work; the field is just absent.

```python
plugins = c.call("list_plugins")
for p in plugins:
    if "manifest" in p:
        # params, inputs, outputs, exchange — read what's there
        ...
    else:
        # fall back to grepping plugin source / inferring from samples
        ...
```

Treat the manifest as a hint, not a contract — fields are free-form,
older plugins won't have one, and what's listed there can drift from
the implementation. If you need ground truth, read the source.

## Escape hatches

- Anything not wrapped → `c.call(name, args)`. Spec at `docs/protocol.md`.
- SDK getting in the way → use `websocket-client` directly, or any other WS client.
- Need to drive the VS Code UI → see `docs/guides/extending-the-ui.md` for command IDs (`xinsp2.imageViewer.applyOp`, etc.).

## Adjacent things this skill doesn't cover

- C++ plugin authoring → `sdk/GETTING_STARTED.md`
- VS Code extension internals → `docs/guides/extending-the-ui.md`
- Project file format → `docs/reference/instance-model.md`
- Test surface → `docs/testing.md`
