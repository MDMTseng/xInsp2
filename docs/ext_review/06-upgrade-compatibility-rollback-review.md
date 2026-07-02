# xInsp2 Upgrade, Compatibility, and Rollback Review

| Field | Value |
|---|---|
| Date | 2026-07-02 |
| Reviewer | Claude (external advisory) |
| Status | Advisory |

Related reviews:

- [`01-project-taste-review.md`](./01-project-taste-review.md) (#6 versioning policy)
- [`00-triage.md`](./00-triage.md) (maintainer lens — findings already triaged are referenced, not re-litigated)
- [`docs/internals/adr-001-host-api-freeze.md`](../internals/adr-001-host-api-freeze.md) (the ABI freeze discipline)

## Scope

This review asks one question across every boundary the product versions
independently: **when two xInsp2 parts meet at different versions, does the
mismatch surface as an explicit, actionable signal — or as silent skew, a
mis-loaded binary, or a config file that can no longer be read?**

It covers:

- the plugin C-ABI: version identity, the freeze discipline, the load-time gate,
  and the carved capability-interface negotiation (`get_interface`);
- the WebSocket protocol: version identity and negotiation across client/backend
  skew;
- project / recipe file format: is there a schema version, and does a
  round-trip through the backend preserve what it does not understand;
- plugin binary compatibility across a backend version change;
- script persistent-state schema versioning;
- the release artifact and the rollback story;
- VS Code extension ↔ backend version skew.

It deliberately does **not** cover reproducible builds, signing, or SBOM — those
belong to the planned *Release Engineering and Supply Chain* review.

Measured against the project's stage: **pre-1.0, first-party only, no external
consumers.** Governance-grade migration tooling is premature and is not asked for
here. What *is* in scope now is **version identity** (can a part name its own
contract) and **negotiation failure modes** (what happens on skew) — because a
silent failure there corrupts a line's config or loads the wrong code, and that
risk exists with a single first-party user.

## Executive Summary

xInsp2's **plugin ABI story is genuinely strong** and is the model the rest of
the system should copy. The `xi_host_api` layout is frozen (ADR-001), the freeze
is enforced by two build-failing guards, the load-time gate refuses both
too-new and too-old plugins with a precise reason, a second gate checks the
yyjson layout, and new capabilities arrive through a versioned
`get_interface(id, version)` door with an LV2-style required/optional handshake.
This is a serious, well-reasoned compatibility design. The Phase 4 break to v11
was deliberate, documented, and authorized.

The weakness is **everything that is not the plugin ABI.** Three boundaries that
matter as much to a running line have essentially no version identity and no
negotiation:

1. **The project file format has no version field, and a plain `save_project`
   rebuilds `project.json` from only two of its keys — silently dropping every
   other top-level key**, including ones the backend itself reads on the next
   open (`runtime`, `parallelism`, `groups`, `toolchain`) and ones another writer
   owns (`params`, `auto_respawn`, `watchdog_ms`). This is the single sharpest
   risk in the review: it is data loss triggered by a normal save, and it is
   exactly the failure mode a cross-version or cross-writer round-trip provokes.

2. **The WebSocket protocol version is a hardcoded constant `abi: 1` that has
   never been bumped** despite several breaking teardowns (the `vars` message,
   core preview frames, and `subscribe`/`unsubscribe` were all removed), is
   **never enforced** by any client, and is **mis-documented** (the protocol
   reference still says the plugin ABI is v9; it is v11). The version field is
   decorative.

3. **Extension ↔ backend skew is unchecked** — the extension logs the backend
   version on `hello` and does nothing with it.

None of these three needs heavy machinery to fix. They need a version *number*
that is actually incremented, a save path that preserves unknown keys, and one
comparison at handshake time. The plugin-ABI layer already proves the team knows
how to do versioning well; these boundaries simply have not received the same
discipline yet.

## Scorecard

| Dimension | Grade | Rationale |
|---|:--:|---|
| Plugin ABI identity & freeze discipline | A− | Frozen layout, two build guards, ADR-001; exemplary |
| Plugin load-time negotiation | A− | Min-compat + max gate, precise refusal reasons, yyjson layout gate |
| Capability interface carving & handshake | B+ | `get_interface` door + LV2 required/optional; probe window is a minor smell |
| Script state schema versioning | B | Explicit version + graceful drop-on-mismatch event; no migration hook (fine pre-1.0) |
| Plugin binary compat across backend roll | B− | Behaviour is correct and loud, but a version roll refuses plugins both ways with no story stated |
| Release packaging & rollback | C | Zip-swap rollback works; one version stamps two "independent" packages; no in-artifact compat manifest |
| Version identity coherence | C | Independent SemVer declared, but the matrix is hand-maintained and the release tool couples packages |
| WS protocol versioning & negotiation | D+ | `abi:1` never bumped, never enforced, mis-documented |
| Project/recipe file format versioning | D | No schema version field; `save_project` drops unknown top-level keys |
| Extension ↔ backend skew handling | D+ | Version received at `hello`, never checked |

## Findings

### 1. `save_project` rebuilds `project.json` from two keys and drops all others

`save_project` reconstructs the entire file from only the script's params and
instances:

- `backend/src/service_main.cpp:2745` — `content = xi::project::build_project_json(params_json, inst_json)`.
- `backend/include/xi/xi_project.hpp:44-52` — `build_project_json` emits an
  object with exactly two keys: `"params"` and `"instances"`. Nothing else is
  read, carried, or merged.

But `project.json` is a multi-key, multi-writer document. The backend itself
reads, on open, top-level keys that `build_project_json` does not emit:
`runtime` (`timer_fps`, `process_priority`), `parallelism`
(`dispatch_threads`, `queue_depth`, `overflow`), `groups`, `toolchain`,
`include_dirs`, `link_libs` (see `docs/reference/ws-protocol.md` §`save_project` /
`open_project`, and the toolchain/runtime handling at `service_main.cpp:2993`,
`:3021`, `:1099`). The VS Code extension owns still others (`params`,
`auto_respawn`, `watchdog_ms`).

The project's own degraded-mode logic *names this exact hazard* as the reason it
refuses a rebuild on a corrupt file: a full rebuild "would otherwise overwrite
the file with a defaults-only document and drop the top-level keys this backend
does not emit but another writer owns (the VS Code extension's `params`,
`auto_respawn`, `watchdog_ms`)" (`docs/reference/ws-protocol.md`,
`open_project` corrupt-file section). The guard exists **only** in degraded
mode. On a healthy file, `save_project` does precisely the destructive rebuild
the degraded path was written to prevent.

The primary UI "Save" path goes through working-copy commit
(`commit_working_copy`, `service_main.cpp:2753`), which mirrors files and is
**not** lossy — so this is not firing on every save in the common flow. But
`save_project` is a first-class, documented command, it is the non-working-copy
save, and it is the obvious command a script/CLI client calls.

#### Consequence

This is the review's top risk because it is a **compatibility failure disguised
as a normal operation**, and it is worst exactly at a version or writer boundary:

- A newer backend introduces a new top-level key; a lateral or older writer's
  `save_project` erases it — the newer feature's config is gone with no error.
- The extension writes `watchdog_ms`; a `save_project` from any client drops it —
  the watchdog silently reverts to default on next open.
- Roll back to an older backend, `save_project`, roll forward: every key the
  older backend didn't know about is now gone.

The runner and headless autostart make this worse — there is no operator watching
to notice the reverted `parallelism` or `runtime` block.

#### Recommendation

Make persistence **merge, not rebuild**. Load the existing `project.json` into a
document, overwrite only the `params` and `instances` subtrees the backend owns,
and write the merged document back — preserving every unrecognized top-level key
verbatim. This is the same "preserve unknown keys" contract the ABI layer already
honours for plugins; apply it to the file format. Until then, document
`save_project` as lossy and steer all writers through the working-copy path.

### 2. Project and recipe files carry no schema version field

`project.json` and `instances/<name>/instance.json` have no format/schema
version marker. `xi_project.hpp:8-18` documents the shape as a bare
`{ "params", "instances" }` object with no version key, and a search of the
project-model and parse paths finds none. Loading is forgiving by construction
(unknown keys tolerated on read, missing keys defaulted, corrupt files
quarantined to `project.json.corrupt-<ts>` — a genuinely good safety net at
`open_project`), but "forgiving" is not "versioned": there is no way for a file
to say *which* format it is, and therefore no hook on which a future migration
could ever hang.

Note the contrast with script persistent state, which **does** version itself
correctly (see Finding 6) — the file format simply never received the same
treatment.

#### Consequence

Today, with one format generation, nothing breaks. But the first format change
(a renamed key, a restructured `instances` shape, a moved `toolchain` block) has
no detectable boundary: an old file opened by a new backend and a new file opened
by an old backend are indistinguishable from a current file by inspection, so the
only available behaviours are "silently mis-read" or "silently default". Combined
with Finding 1, the migration story is not just absent — a round-trip actively
destroys the keys a migration would need to read.

#### Recommendation

Add a single top-level `"schema": N` (or `"format_version"`) to `project.json`
and to `instance.json` now, while there is exactly one version to stamp. Cost is
one integer written and one read. It does not require building a migration engine
— it requires reserving the **identity** so that when a migration is eventually
needed, the file can be recognized rather than guessed. Pre-1.0, "unknown future
version → refuse with a clear message" is an acceptable migration policy; an
un-versioned file cannot even do that.

### 3. The WebSocket protocol version is a decorative constant

The protocol advertises a version at two points, both hardcoded:

- `backend/src/service_main.cpp:1237` — `hello` event carries `"abi":1`.
- `service_main.cpp:2248` — `cmd:version` returns `"abi":1`.

This `abi` has been `1` across breaking wire changes: the `vars` message, the
core binary preview frame, and the `subscribe`/`unsubscribe` commands were all
**removed** (`docs/reference/ws-protocol.md` §"Removed" blocks throughout), and
`run_finished`/`metrics` field renames are staged. The protocol reference itself
states the intended contract — "breaking schema changes bump the server's
version string and the `hello` event's `abi` field" (`ws-protocol.md:18-25`) —
and then, in the same paragraph, admits "There is no enforced version gate today
— a client MAY compare `hello.data.abi` … but the shipped clients only log it."
So the field is neither incremented on breaks nor checked by anyone.

The documentation is also stale in a way that compounds the confusion:
`ws-protocol.md:375` says the plugin ABI struct version is "9" — it is **11**
(`xi_abi.h:150`). A reader cross-referencing the two ABIs gets a wrong number.

#### Consequence

Client/backend skew is silent. An old client against a new backend sees missing
frames or `ok:false` on retired commands with no version signal telling it *why*;
a new client against an old backend gets unexpected shapes. The one field that
could turn this into a clean, actionable "protocol vN required, server speaks
v1" refusal is inert.

#### Recommendation

Two cheap steps: (a) actually **bump** the WS `abi` integer when the wire shape
breaks — the `vars`/preview teardown was a v2 event that never got a number; and
(b) have the shipped clients (extension, `ui-components`, Python) compare
`hello.data.abi` against a known-minimum and surface a visible, single-line
warning on mismatch (not a silent log). Enforcement can stay advisory pre-1.0 —
but the number must first be real. Fix the `v9`→`v11` doc reference in the same
pass.

### 4. Extension ↔ backend version skew is unchecked

On `hello`, the extension does exactly one thing with the backend version:

- `vscode-extension/src/extension.ts:1102-1103` —
  `output.appendLine(\`[xinsp2] backend v${msg.data?.version}\`)`.

There is no comparison against an expected/minimum backend version, and no check
of the WS `abi`. The extension spawns whatever `xinsp2.backendExe` points at
(README install step), which on a hand-updated machine can easily be a stale
backend next to a freshly installed VSIX — the exact skew the independent-version
policy makes possible.

#### Consequence

A developer updates the extension but not the backend binary (or vice versa) and
gets subtly wrong behaviour — a command the new extension sends that the old
backend answers `ok:false`, or a frame shape the old extension can't parse — with
no diagnostic pointing at version skew. Debugging time is spent on a "bug" that
is really a mismatch.

#### Recommendation

At `hello`, compare `data.version` (and, once Finding 3 is real, `data.abi`)
against the extension's bundled known-compatible values from the README matrix.
On mismatch, show a dismissable warning naming both versions and the known-good
row. This is a handful of lines and turns a class of silent skew into a one-line
answer.

### 5. The release process couples packages the versioning policy calls independent

The README declares four independently-versioned packages with per-package SemVer
tracks (README §"Versioning & compatibility"). The release builder contradicts
this:

- `tools/build_release.mjs:30-37` reads a single `XINSP2_VERSION` from
  `backend/CMakeLists.txt` and uses it for the whole release name.
- `:86-87` stamps the **VSIX filename** with that same backend version
  (`xinsp2-${VERSION}.vsix`), even though the extension's real version lives
  independently in its `package.json` (which is what `vsce` writes into the VSIX
  manifest). The filename therefore asserts the backend's version for an artifact
  whose internal version is something else.

The release is also the rollback unit, and it carries **no machine-readable
record of the compatible set** it was built from — the "known-compatible matrix"
lives only in README prose, not inside the artifact.

#### Consequence

The independent-versioning promise is undermined at the one place it becomes
concrete. A `.vsix` filename that lies about the extension version is a real
source of confusion when reconstructing "what was deployed" during a rollback or
incident. And because the compatible set is not recorded in the artifact, the
only way to know which backend/extension/ABI a given zip represents is to trust
the README of the commit it was cut from.

#### Recommendation

Either (a) commit to a single product version and drop the independent-versioning
language, or (b) make the release honestly independent: name the VSIX from the
extension's own `package.json` version, and write a small `manifest.json` into
the release root recording each package's version and the ABI/protocol numbers
(the known-compatible row, machine-readable). Given the spine's "several packages
that version independently" stance, (b) is the smaller lie to carry. Rollback then
becomes "read the manifest of the zip you're rolling to."

### 6. Script persistent-state schema is versioned well — note it as the model

This is a **positive** finding, recorded because it is the pattern Findings 1–3
should imitate. Script state carries an explicit schema version and negotiates a
mismatch gracefully:

- The script declares `XI_STATE_SCHEMA(N)` (`ws-protocol.md:232-241`).
- On hot-reload the backend compares the new DLL's
  `xi_script_state_schema_version()` against the persisted one
  (`service_main.cpp:2591-2601`), and on mismatch **drops** the incompatible
  state and emits a `state_dropped` event carrying `{old_schema, new_schema}`
  (`:2648-2650`) rather than default-filling stale bytes into a new shape.

There is no migration *function* (mismatch drops rather than transforms), which
is entirely appropriate pre-1.0. The point is that this boundary has a version, a
comparison, and an explicit, observable outcome on skew. That is exactly what the
project file, the WS protocol, and the extension handshake lack.

#### Recommendation

None needed. Use this as the template: version, compare, emit an explicit event
on mismatch.

### 7. Plugin binary compatibility across a backend roll is correct but unstated

The load gate is genuinely well built (`xi_cabi_adapter.hpp:54-95`,
`plugin_abi_compatible`): a plugin requesting a newer ABI than the host is
refused with a reason; one older than `XI_ABI_MIN_COMPAT` is refused with a
rebuild instruction; a pre-versioning plugin is treated as v1 and thus refused;
and a yyjson-layout mismatch is refused unless the manifest opts into
`json_fallback`. This is loud, safe, and clearly messaged — a real strength.

The gap is that the **rollback consequence** of the v11 break is nowhere stated
for operators. With `XI_ABI_VERSION = 11` and `XI_ABI_MIN_COMPAT = 11`
(`xi_abi.h:150,156`), the window is exactly one version wide: a v11-built plugin
is **refused by a rolled-back v10 host** (too new), and a v10 plugin is **refused
by the v11 host** (below min-compat). So rolling the backend across the v11
boundary refuses first-party plugin binaries **in both directions** — every
plugin must be rebuilt against the target's ABI as part of the roll.

There is also a smaller truth-in-labeling issue: plugin export stamps
`"abi_version"` into the generated `plugin.json`
(`xi_plugin_export.hpp:120-137`) and the comment claims a target backend "can
detect the mismatch on scan." It cannot — the manifest parser
(`xi_pm_parse.hpp`) never reads `abi_version`; the only enforced gate is the
DLL's runtime `xi_plugin_abi_version` export. The stamped field is decorative.

#### Consequence

For the current authorized break this is fine — all first-party plugins rebuild
in-tree — but an operator who treats "roll back the backend zip" as a complete
rollback will find the reverted host refusing the plugins that shipped with the
newer release, with no pre-flight warning. And anyone trusting the manifest's
`abi_version` for an off-line compatibility check is trusting a field nothing
validates.

#### Recommendation

State the rule in the rollback story: **a plugin binary is only guaranteed to
load on the ABI version it was built against; crossing an ABI break requires a
rebuild, and the shipped plugin set is part of the release unit, not
independently rollable.** Either wire the manifest `abi_version` into a scan-time
pre-check (so the decorative field earns its comment) or drop it and rely solely
on the DLL export it duplicates.

### 8. The capability handshake probe window is a minor smell

`host_publishes_iface` answers a "≥ min" capability query by probing exact
versions in a 256-wide loop from `min` (`xi_cabi_adapter.hpp:176-184`). It works
and is bounded, but it encodes "additive, frozen (id, vN)" as a linear scan
rather than asking the host for the highest published version of an id. It is
correct today; it will read oddly if interface version counts ever grow or become
sparse.

#### Consequence

Negligible at present — purely a design-clarity note.

#### Recommendation

Consider a `get_interface_max(id)` or a documented "versions are dense from 1"
invariant so the probe is a lookup, not a scan. Low priority.

## Prioritized Roadmap

### P0 — Stop the silent data loss (this quarter)

1. Make `save_project` **merge** into the existing `project.json`, preserving
   unknown top-level keys (Finding 1). This is the one finding that destroys data
   during a normal operation.
2. Add a `"schema": N` field to `project.json` and `instance.json` (Finding 2) —
   reserve the identity now, while there is one version to stamp.

### P1 — Make version identity real (near-term)

3. Bump the WS `abi` integer for the wire breaks that already happened, and fix
   the `v9`→`v11` doc reference (Finding 3a).
4. Have the shipped clients compare `hello.data.abi` / `version` and surface a
   visible mismatch warning; add the same check to the extension's `hello`
   handler (Findings 3b, 4).

### P2 — Make the release self-describing (when convenient)

5. Name the VSIX from the extension's own version and write a machine-readable
   `manifest.json` (versions + ABI + protocol) into the release root (Finding 5).
6. Document the plugin-binary rollback rule and either wire or drop the manifest
   `abi_version` field (Finding 7).

### P3 — Polish

7. Replace the capability probe scan with a max-version lookup or a documented
   density invariant (Finding 8).

## Decision Checklist

### File format

- Does a save preserve every top-level key the saving component does not own?
- Can a file state which format version it is?
- On an unknown future version, does load refuse with a clear message rather than
  default silently?

### Protocol

- Is the WS `abi` incremented when the wire shape breaks?
- Do the shipped clients compare it and surface a mismatch?
- Do the two ABIs (WS protocol vs plugin struct) carry the correct, distinct
  numbers in the docs?

### Plugin ABI

- Does a load refusal name the requested vs provided version and the fix? (Yes.)
- Is the freeze enforced by a build-failing guard? (Yes.)
- Is the "rebuild required across an ABI break" rule stated in the rollback
  story?

### Release & rollback

- Does the artifact record its own known-compatible set (versions + ABI +
  protocol)?
- Does the VSIX filename match the extension's actual version?
- Is "roll back the zip" documented as also rolling back the bundled plugins?

### Skew

- Does the extension check the backend version it connects to?
- Is there a single, machine-readable source of the compatible matrix, or only
  README prose?

## Final Judgment

xInsp2 has one exemplary compatibility surface and several neglected ones. The
plugin ABI — frozen layout, build-enforced freeze guards, a precise two-sided
load gate, a yyjson-layout check, and a versioned capability door with an
LV2-style handshake — is among the best-considered parts of the whole codebase,
and the Phase 4 break to v11 was handled exactly as a deliberate major break
should be. The script state-schema mechanism shows the same discipline on a
smaller surface.

The problem is that this discipline stopped at the ABI. The project file format
has no version and, worse, a normal `save_project` rebuilds it from two keys and
drops the rest — a data-loss bug that a cross-version or cross-writer round-trip
provokes directly, and the codebase's own degraded-mode guard proves the team
already knows the hazard exists. The WS protocol version is a constant that has
never moved through multiple breaking teardowns and that no one checks. The
extension receives the backend version and ignores it.

None of this is a call for governance machinery — the triage lens correctly
defers that. These are the opposite: the *cheap* half of versioning, the identity
and the single comparison, applied to boundaries that currently have neither. The
team has already built the expensive, careful version of this for plugins. The
recommendation is simply to spend a fraction of that effort on the file format,
the protocol, and the handshake — starting with the `save_project` merge, because
that one silently destroys configuration today.
