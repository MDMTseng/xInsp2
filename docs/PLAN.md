# docs-v2 — a fresh, consolidated documentation plan

A clean-room redesign of the doc tree. The current `docs/` stays untouched and
authoritative until this is built out and we cut over. This file is the PLAN —
the target structure, the principles, and the source-mapping — not the content.

---

## Why redo it

An audit of `docs/` (2026-06) found it's *broad* but not *tight*:

- **~40 files, organised by topic not by reader.** A newcomer, a plugin author,
  and a maintainer all have to hunt across the same flat lists.
- **`design/` mixes shipped and not-scheduled.** `data-layer`, `io-types-and-na`,
  `emitter-fetch-model`, `dispatch-groups` are SHIPPED design-of-record; sitting
  next to `run-result`, `interactive-tool-registry`, `production-hmi` (sketches).
  The reader can't tell what's real.
- **Duplication + drift.** `status.md` re-states what `architecture.md` says and
  drifts (it lagged the yyjson/γ-4 pivot). `host_api.md` lagged ABI v4.
- **Misleading names / dead files in-tree.** `wire-format-msgpack.md` (now
  `data-layer.md`), `ipc-shm.md` (REMOVED), `comms-gateway.md` (Superseded) still
  live among current docs.

## Principles

1. **Organise by reader intent, not by topic.** Five questions: *what is this? ·
   how do I build/run it? · how do I author with it? · what's the exact contract?
   · how does it work inside?* Each has one home.
2. **One authoritative source per fact.** No `status.md` ↔ `architecture.md`
   restatement. Status becomes a thin roadmap that *links*, never re-describes.
3. **Shipped vs not-scheduled is a hard split, not a label.** Shipped subsystems
   live in `internals/` (design-of-record). Unscheduled ideas live in `roadmap/`.
   A doc graduates from `roadmap/` to `internals/` when it ships.
4. **Tight over complete.** `io-types-and-na.md` is the quality bar: concise,
   concrete, code-anchored. ~20 files, each earning its place.
5. **Dead docs go to `archive/`, not in-tree.** REMOVED/Superseded content is
   archived, with a one-line tombstone where readers might still look.

---

## Target structure

```
docs-v2/
  README.md                  # one-page index → everything below

  overview.md                # mental model + architecture-in-one-picture +
                             #   the core nouns (Record, Image, plugin, script,
                             #   instance, trigger). Read once, ~20 min.

  guides/                    # task-shaped — pick the verb
    build-and-run.md         # set up a machine, build, run on day one
    write-a-script.md        # the inspection script: lifecycle + every primitive
    write-a-plugin.md        # author a plugin (in-project + standalone) + its UI
    debug.md                 # what's caught, crash reports, attach a debugger
    extend-the-ui.md         # add a command/tree/webview to the extension
    deploy.md                # production: boot order, AOT bundle, working-copy

  reference/                 # exact contracts — argument shapes, invariants
    c-abi.md                 # plugin DLL exports + xi_host_api table (one ABI doc)
    data-types.md            # Record + Image + typed I/O + NA, at the boundary
    ws-protocol.md           # WebSocket commands / replies / events / frames
    instances.md             # instance load/persist/registry/teardown + schema

  internals/                 # design-of-record for SHIPPED subsystems (the "how")
    data-layer.md            # yyjson-only + in-process doc pass-by-pointer + γ-4
    dispatch.md              # trigger bus + emit/fetch + dispatch groups
    fe-be.md                 # FE supervisor over BE, safe-state, crash history
    typed-io.md              # nominal types over Record + NA + provenance

  roadmap/                   # NOT scheduled — sketches; graduate to internals/
    README.md                # the roadmap + shipped-status summary (was status.md)
    run-result.md
    interactive-tool-registry.md
    production-hmi.md
    linux-port.md

  archive/                   # historical snapshots + removed subsystems
    (ipc-shm, comms-gateway, shm-*, architecture-review-*, fe-test-rounds, ...)
```

~22 living files (vs ~40), each with a single reader and a single home.

---

## Source mapping (new ← current)

| New file | Built from current docs |
|---|---|
| `overview.md` | `getting-started.md` + the conceptual half of `architecture.md` |
| `guides/build-and-run.md` | `guides/install.md` + run-on-day-one from `getting-started.md` |
| `guides/write-a-script.md` | `guides/writing-a-script.md` |
| `guides/write-a-plugin.md` | `guides/adding-a-plugin.md` + `guides/plugin-ui-conventions.md` |
| `guides/debug.md` | `guides/debugging.md` |
| `guides/extend-the-ui.md` | `guides/extending-the-ui.md` |
| `guides/deploy.md` | `design/deployment.md` + `guides/project-working-copy.md` |
| `reference/c-abi.md` | `reference/plugin-abi.md` + `reference/host_api.md` (merged) |
| `reference/data-types.md` | `reference/image-io.md` + Record/boundary parts of `architecture.md` + `design/io-types-and-na.md` (the contract surface) |
| `reference/ws-protocol.md` | `protocol.md` |
| `reference/instances.md` | `reference/instance-model.md` |
| `internals/data-layer.md` | `design/data-layer.md` (drop the retained-msgpack appendix to an archive note) |
| `internals/dispatch.md` | `design/emitter-fetch-model.md` + `design/dispatch-groups.md` + trigger-bus parts of `architecture.md` |
| `internals/fe-be.md` | `design/fe-be-split.md` (+ test-plan as a linked appendix) |
| `internals/typed-io.md` | `design/io-types-and-na.md` (the mechanics half) |
| `roadmap/README.md` | `status.md` (slimmed to status + links, no re-description) |
| `roadmap/*` | `design/{run-result,interactive-tool-registry,production-hmi,linux-port}.md` |
| `archive/*` | current `archive/*` + `reference/ipc-shm.md` + `design/comms-gateway.md` |

Notes:
- `reference/c-abi.md` is the biggest merge — `plugin-abi.md` (exports) and
  `host_api.md` (services) are two halves of one contract; readers bounce between
  them today. Merge, keep the ABI version history in one place.
- `internals/dispatch.md` collects everything about "how a trigger becomes a run":
  bus correlation, emit/fetch id-dispatch, dispatch-group priority. Today these
  are three docs + scattered architecture prose.
- `roadmap/README.md` replaces `status.md` as a *thin* roadmap: the shipped table
  links to the relevant `internals/` or `reference/` doc instead of restating it.

---

## Migration plan (phased, low-risk)

`docs/` stays the source of truth the whole time; `docs-v2/` is built and reviewed
in parallel, then swapped in one move.

- **P0 — this plan.** Agree the structure + mapping. (this file)
- **P1 — skeleton.** Create every target file with a heading + a one-line scope +
  a `<!-- source: ... -->` pointer. No content yet — validates the shape.
- **P2 — fill, doc-by-doc.** Port + tighten each file from its sources. Reference
  + internals first (highest drift risk), then guides, then overview, then roadmap.
  Each ported file ends with the old one(s) it supersedes noted for the cutover.
- **P3 — cutover.** Move dead docs to `archive/`, `git mv docs-v2/* docs/`
  (replacing), update cross-repo references (root README, CONTRIBUTING, source
  comments), drop a redirect tombstone for any renamed path people may have linked.
- **P4 — guardrail.** Update the "When to update what" table so the one-home rule
  is enforced going forward.

## Open questions (decide before P1)

1. Folder name on cutover: keep `docs/` (replace contents) — assumed yes.
2. `reference/data-types.md` — is merging Image-io + Record + typed-IO too much
   in one file, or is one "what crosses the boundary" doc the right grouping?
3. Keep `getting-started.md` as a separate front door, or fold it into
   `overview.md` + `guides/build-and-run.md`? (plan assumes fold)
4. How aggressively to prune the retained-MessagePack appendix in `data-layer.md`
   — archive-note vs keep-inline.

---

## P2 status (live)

A rule emerged while filling: **already-current, well-shaped docs are MOVED at
cutover, not rewritten in P2** — P2 only rewrites what genuinely needs merging,
tightening, or de-staling.

**Rewritten + tightened (done):**
- `reference/c-abi.md` ← plugin-abi + host_api (merged; de-staled to ABI v4 + doc fields)
- `reference/data-types.md` ← image-io + Record + io-types contract
- `reference/instances.md` ← instance-model (tightened, cross-refs)
- `internals/data-layer.md` ← data-layer (from final impl; msgpack appendix dropped)
- `internals/dispatch.md` ← emitter-fetch + dispatch-groups + trigger (3-way merge)
- `internals/fe-be.md` ← fe-be-split
- `internals/typed-io.md` ← io-types-and-na (mechanics half)
- `overview.md` ← getting-started + architecture (concepts; links updated)
- `roadmap/README.md` ← status.md (thinned to links, not restatement)

**Cutover-MOVE (source already current — `git mv` + light tighten at P3):**
- `reference/ws-protocol.md` ← protocol.md (verify command list vs service_main.cpp)
- `guides/write-a-script.md` ← writing-a-script.md
- `guides/debug.md` ← debugging.md
- `guides/extend-the-ui.md` ← extending-the-ui.md
- `roadmap/{run-result,interactive-tool-registry,production-hmi,linux-port}.md` ← same-named design/ (forward-looking, port as-is)

**Cutover-MERGE (two current sources stitched at P3 + light tighten):**
- `guides/build-and-run.md` ← install.md + getting-started §5 (build/run)
- `guides/write-a-plugin.md` ← adding-a-plugin.md + plugin-ui-conventions.md
- `guides/deploy.md` ← deployment.md + project-working-copy.md

So P2's *rewrite* work is complete; the remaining 11 files are mechanical
move/merge that belong with the P3 cutover (they'd only drift if rewritten now
against still-live `docs/`).
