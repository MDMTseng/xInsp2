# Roadmap & status

A **thin** status: what's shipped (by area, linking to the doc that describes it —
never restating), the locked-in decisions, and the not-scheduled sketches in this
folder. This replaces the old `status.md` single-source-of-truth that drifted by
re-describing other docs.

## Shipped (master)

| Area | Where it's documented |
|---|---|
| C ABI, plugin loading, instances | [`../reference/c-abi.md`](../reference/c-abi.md), [`../reference/instances.md`](../reference/instances.md) |
| Record / Image / typed I/O + NA | [`../reference/data-types.md`](../reference/data-types.md), [`../internals/typed-io.md`](../internals/typed-io.md) |
| yyjson data layer + γ-4 doc refcount | [`../internals/data-layer.md`](../internals/data-layer.md) |
| Trigger bus + emit/fetch + dispatch groups | [`../internals/dispatch.md`](../internals/dispatch.md) |
| FE/BE supervisor + safe-state + crash | [`../internals/fe-be.md`](../internals/fe-be.md) |
| WS API | [`../reference/ws-protocol.md`](../reference/ws-protocol.md) |
| Sharded refcounted ImagePool, SEH crash isolation, auto-respawn, atomic JSON writes, skip-bad-instance, compile diagnostics | [`../internals/fe-be.md`](../internals/fe-be.md), [`../guides/debug.md`](../guides/debug.md) |
| In-project plugins, hot reload, export-plugin, SDK scaffold, per-plugin cert, plugin webviews | [`../guides/write-a-plugin.md`](../guides/write-a-plugin.md) |
| `xi::state` / `xi::async` / `xi::breakpoint` / hot-reload / script DLL versioning | [`../guides/write-a-script.md`](../guides/write-a-script.md) |
| Recording + replay, interactive viewer, variants/compare, remote mode | [`../guides/extend-the-ui.md`](../guides/extend-the-ui.md) |

7 plugins shipped under `plugins/`; SDK demo plugins under `sdk/examples/`. Full
test surface: [`../../docs/testing.md`](../../docs/testing.md) (→ `testing.md` after cutover).

## Removed (don't look for these)

- **Process isolation + SHM mesh** (2026-05) — all plugins run in-process; see
  [`../internals/fe-be.md`](../internals/fe-be.md). The `instance.json` `isolation`
  field is accepted-but-ignored.
- **cJSON, the explored MessagePack/CWPack wire, the `xinsp-comms` gateway** — see
  [`../internals/data-layer.md`](../internals/data-layer.md) (data) +
  `../internals/fe-be.md` (PLC is a plugin + the FE safe-state sink).

## Decision log (locked-in)

- **Everything over WS framing** (no BPG). Backend is a standalone exe (no N-API).
- **No graph-authoring editor.** The script is the source of truth; a read-only
  pipeline graph view exists for visualization only.
- **C++ via MSVC `cl.exe`**, versioned DLL naming for Windows lock survival. No
  Cling/ClangREPL.
- **Stable C ABI; no C++ types cross the boundary.**
- **Dependency-free host** — only yyjson + stb_image_write vendored; OpenCV / IPP /
  turbojpeg optional.
- **VS Code is the IDE; no in-house editor. Headless backend** drives from any WS
  client.

## Not scheduled (sketches in this folder)

Forward-looking; each graduates into `reference/` or `internals/` when it ships.

- [`run-result.md`](./run-result.md) — per-run signed verdict record (the
  `xi::result` API ships; the full status-band + HMI/PLC wiring is the sketch).
- [`interactive-tool-registry.md`](./interactive-tool-registry.md) — shared
  image-viewer panel for plugins needing GUI setup.
- [`production-hmi.md`](./production-hmi.md) — operator dashboard composer (v1.0
  RUN mode built; compose mode + the rest forward-looking).
- [`linux-port.md`](./linux-port.md) — cross-platform port (revisit after a stable
  Windows release).
