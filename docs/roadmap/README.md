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
| Trigger bus + `emit_record` + dispatch groups | [`../internals/dispatch.md`](../internals/dispatch.md) |
| FE/BE supervisor + crash history | [`../internals/fe-be.md`](../internals/fe-be.md) |
| WS API (incl. `get_instance_def` symmetric read + `cmd:run` record injection) | [`../reference/ws-protocol.md`](../reference/ws-protocol.md) |
| Sharded refcounted ImagePool, SEH crash isolation, auto-respawn, atomic JSON writes, skip-bad-instance, compile diagnostics | [`../internals/fe-be.md`](../internals/fe-be.md), [`../guides/debug.md`](../guides/debug.md) |
| In-project plugins, hot reload, export-plugin, SDK scaffold, plugin webviews | [`../guides/write-a-plugin.md`](../guides/write-a-plugin.md) |
| `xi::state` / `xi::async` / `xi::breakpoint` / hot-reload / script DLL versioning | [`../guides/write-a-script.md`](../guides/write-a-script.md) |
| Replay (buffer_replay plugin), interactive viewer, remote mode | [`../guides/extend-the-ui.md`](../guides/extend-the-ui.md) |

7 plugins shipped under `plugins/`; SDK demo plugins under `sdk/examples/`. Full
test surface: [`../../docs/testing.md`](../../docs/testing.md) (→ `testing.md` after cutover).

## Removed (don't look for these)

- **Process isolation + SHM mesh** (2026-05) — all plugins run in-process; see
  [`../internals/fe-be.md`](../internals/fe-be.md). The `instance.json` `isolation`
  field is accepted-but-ignored.
- **cJSON, the explored MessagePack/CWPack wire, the `xinsp-comms` gateway** — see
  [`../internals/data-layer.md`](../internals/data-layer.md) (data) +
  `../internals/fe-be.md` (PLC is a plugin; line-safe is its own sidecar process).
- **The multi-verb dispatch surface** (2026-06, ABI v6) — `emit_trigger` /
  `emit_resource` / `fetch_image` / `fetch_resource` / `emit_dispatch`, trigger-bus
  correlation policies, the host record/replay recorder, and the legacy
  `xi::ImageSource` + `grab()` pull-model. Collapsed to ONE verb `emit_record`;
  multi-cam = a gathering plugin, replay = a `buffer_replay` plugin. See
  [`../internals/dispatch.md`](../internals/dispatch.md).
- **Plugin certification** (cert.json / baseline / certify-on-load gate /
  `recertify_plugin`) — plugins now load trusted (speed-first); no cert gate or UI.
- **FE-brokered PLC safe-state** (the `--safe-state` flag + `set_safe_state` ABI
  verb + FE→PLC delivery sink) — replaced by a comms plugin's crash-watching
  sidecar; see `../internals/fe-be.md`.
- **ws commands `preview_instance` / `process_instance` / `compare_variants`** —
  redundant with `cmd:run`.

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

## Known issues / open bug list

[`known-issues.md`](known-issues.md) — what's still open after the 2026-06 hardening
campaign (latent/edge correctness, observability gaps, design DO-LATERs, won't-fix
rationale, and the pre-existing backlog), tagged by design theme.

## Not scheduled (sketches in this folder)

Forward-looking; each graduates into `reference/` or `internals/` when it ships.
Several are platform requests from RFC
[#65](https://github.com/MDMTseng/xInsp2/issues/65) (FR-1 shipped via
`emit_record` + `current_trigger().meta()`; FR-7 mostly via the instance docs).

- [`config-bundles-and-orchestration.md`](./config-bundles-and-orchestration.md) —
  object/version config switching as a controller plugin over a minimal
  "command-other-instances" host_api; version branch-graph view (RFC #65 FR-2/FR-4).
- [`run-result.md`](./run-result.md) — per-run signed verdict record (the
  `xi::result` API ships; the full status-band + HMI/PLC wiring is the sketch).
- [`webui-and-ui-export.md`](./webui-and-ui-export.md) — a Web Component library
  (auto-webui from a control-descriptor schema + custom escape hatch) and
  section-based UI export in two shapes (status-observation vs full-app extract).
  Absorbs the interactive-tool-registry sketch.
- [`interactive-tool-registry.md`](./interactive-tool-registry.md) — draw-on-image
  teach tools (polygon/ROI/fiducial); now a sub-part of `webui-and-ui-export.md`.
- [`production-hmi.md`](./production-hmi.md) — operator dashboard composer (v1.0
  RUN mode built; compose mode + the rest forward-looking).
- [`linux-port.md`](./linux-port.md) — cross-platform port (revisit after a stable
  Windows release).
