# xInsp2 Project Taste Review

| Field | Value |
|---|---|
| Review date | 2026-07-02 |
| Scope | Product, UI, documentation, operations, security, and repository coherence |
| Status | Advisory review |

## Scope

This review asks a narrower question than "does the system work?":

> Does the project make deliberate, coherent choices, and are those choices
> visible at every layer of the product?

Here, **taste** does not mean decoration or personal preference. It means:

- choosing a small number of strong ideas;
- making the important path obvious;
- removing concepts that users should not need to understand;
- expressing the same principles in architecture, API, UI, docs, operations,
  and repository structure;
- preferring structural constraints over rules that rely on memory;
- spending complexity only where it creates clear user value.

This is a repository-level review based on the current source, documentation,
tests, extension manifest, plugin UIs, and HMI implementation. It is not a
formal security audit or a usability study with production operators.

## Executive Summary

xInsp2 has stronger architectural taste than most projects at this stage. Its
best decisions are clear and defensible:

- C++ script-first authoring instead of a graph editor;
- a small set of authoring nouns: `Instance`, `Param`, `Record`, and result;
- a frozen C ABI with explicit escape hatches;
- a minimal compute core and plugin-oriented capability model;
- zero-copy and crash behavior treated as first-order design constraints;
- unusually serious behavioral and integration testing.

The main weakness is that these principles are not yet expressed consistently
outside the backend. The product currently feels like a precise engine with
several separately evolved control surfaces around it.

The central recommendation is therefore not "redesign everything." It is:

> Apply the same discipline used to minimize the backend core to the user
> workflow, extension architecture, HMI information model, documentation
> maintenance, and release process.

## Scorecard

| Area | Score | Assessment |
|---|---:|---|
| Core architecture | 8/10 | Strong constraints and explicit trade-offs |
| Authoring vocabulary | 8/10 | Memorable primitives, but some legacy vocabulary remains |
| API and protocol design | 7/10 | Carefully documented, still broad and manually synchronized |
| Reliability engineering | 8/10 | Strong crash, race, and end-to-end attention |
| Product workflow | 6/10 | Capable, but too many equally visible operations |
| Extension architecture | 4/10 | Front-end orchestration has become a second monolith |
| Production HMI | 4/10 | Functional dashboard, not yet operator-centered |
| Visual system | 4/10 | Host-aware but generic and weakly systematized |
| Documentation system | 7/10 | Excellent intent and coverage guard, visible semantic drift |
| Repository/release coherence | 5/10 | Multiple packages and versions rely partly on manual sync |
| Security posture | 5/10 | Auth exists, deployment guidance does not make the safe path dominant |
| Overall coherence | 6/10 | Good local decisions do not yet read as one product |

## Design Guardrails

The following are distinguishing choices, not deficiencies:

1. **Keep script-first authoring.** A graph can explain execution, but should not
   become a second source of truth.
2. **Keep VS Code as the development host.** Building a custom IDE would spend
   complexity without improving the core proposition.
3. **Keep plugin code in-process where zero-copy performance requires it.** The
   crash trade-off is understood and has a supervisor-based recovery model.
4. **Keep the C ABI narrow and frozen.** Convenience should not casually expand
   the host table.
5. **Keep Windows-first honest.** "Portable later" is better than pretending the
   present toolchain and operational model are platform-neutral.
6. **Keep advanced capabilities available through commands.** The recommendation
   is to reduce their prominence, not remove expert access.

## Findings

### 1. Product idea and positioning

The strongest product promise is:

> Write one C++ inspection script and get hot reload, live tuning, plugin
> composition, crash recovery, and production execution.

The README and extension currently let the feature inventory compete with this
idea. The extension exposes 33 commands. Recording, continuous operation,
compilation, project management, plugin development, pipeline visualization,
image tools, and backend maintenance all appear important.

This forces newcomers to understand implementation boundaries before they can
act, and it leaves experts unsure which workflow is canonical.

#### Recommendation

Define one canonical loop:

1. Open or create a project.
2. Add and configure an instance.
3. Write the script.
4. Run against a frame.
5. Inspect the result and tune parameters.
6. Promote the same project into continuous or production execution.

Collapse the visible product into three top-level intents:

- **Run:** compile if needed, execute the appropriate mode, show the result;
- **Inspect:** images, outputs, result, timing, and pipeline explanation;
- **Configure:** project, instances, plugins, and deployment settings.

Rebuild, rescan, restart, export plugin, recording, and self-test commands can
remain searchable without competing with the primary loop.

#### Acceptance criteria

- First result does not require learning `run` versus `start` versus `compile`.
- Primary actions use user intent rather than infrastructure verbs.
- The README shows a useful outcome before architecture detail.

### 2. Conceptual integrity and extension architecture

The backend has a written spine: speed-first, minimal core, and capability as
plugins. The same standard is not applied to the VS Code extension.
`extension.ts` is 2,864 lines and its `activate()` function owns lifecycle,
commands, project workflow, graph rendering, image integration, plugin workflow,
recording, and backend supervision.

This is more than a maintainability smell. It creates an architectural
contradiction: backend complexity must justify entering the core, while
front-end complexity can accumulate in activation.

#### Recommendation

Establish an extension spine:

1. Activation is a thin composition root.
2. Connection, project, compilation, run state, and selection each have one
   structural owner.
3. Feature modules receive capabilities instead of reproducing global state
   checks.
4. Webview generation and protocol operations live outside command registration.

Suggested boundaries:

```text
extension/
  activate.ts
  session/backend-session.ts
  project/project-controller.ts
  run/run-controller.ts
  instances/instance-feature.ts
  plugins/plugin-feature.ts
  recording/recording-feature.ts
  graph/pipeline-graph-feature.ts
  views/
```

Extract one complete feature at a time rather than rewriting the extension.
Recording or plugin management are good first candidates.

#### Acceptance criteria

- `activate()` reads as wiring rather than the product implementation.
- A feature can be tested without activating the entire extension.
- Readiness and connection rules are not reproduced in each command.

### 3. Information architecture and interaction

Current operations are organized by subsystem: backend, project, plugin,
instance, viewer, and recording. Users think in tasks:

- Why did this part fail?
- Use this image to tune the detector.
- Run continuously.
- Make this configuration production-ready.

#### Recommendation

Organize surfaces by phase:

| Phase | Primary surface | Secondary actions |
|---|---|---|
| Build | Project and instance setup | plugin folder, toolchain diagnostics |
| Tune | Script, frame, params, result | image tools, pipeline explanation |
| Validate | Repeatable runs and replay | recording, comparison, reports |
| Operate | Continuous state and HMI | restart, diagnostics, deployment |

Use progressive disclosure. Show one primary action, place contextual actions
beside their object, and keep repair commands in overflow or the command palette.

The HMI composer currently exposes implementation terms such as split direction,
card type, variable binding, and JSON export. Prefer semantic blocks:

- Current verdict
- Last failure
- Line state
- Throughput
- Yield
- Alarm history
- Inspection output
- Station health

The layout tree can remain available as an advanced inspector.

### 4. Production HMI

The HMI currently prioritizes dashboard flexibility before operator action. Its
built-in cards cover verdict, calculated throughput, yield, and dispatch group
utilization. Dispatch groups are engineering diagnostics. The primary operator
questions are:

1. Is the line safe and connected?
2. Is inspection active, stopped, or degraded?
3. Was the last part OK, NG, NA, or a system failure?
4. If action is required, what should the operator do?
5. What evidence explains the state?

#### Recommendation: define the state model first

| State | Meaning | Required treatment |
|---|---|---|
| Starting | Not ready to judge parts | Neutral progress plus reason |
| Ready | Configured but not running | Clear readiness and start policy |
| Running | Inspections are current | Stable live state and freshness |
| Degraded | Running with reduced capability | Persistent amber explanation |
| Stopped | Intentionally stopped | Distinct from failure |
| Faulted | Cannot inspect safely | Dominant fault and recovery action |
| Disconnected | HMI cannot establish truth | Never preserve stale green |

Separate pages by role:

- **Operate:** state, verdict, takt, last NG, alarms;
- **Diagnose:** station health, dispatch, compile state, logs;
- **Quality:** yield, trends, defect classes, sample images;
- **Configure:** layout and integration settings.

#### Visual direction

Aim for a **precision instrument**, not a generic dark dashboard:

- reserve saturated color for state, alarm, and selection;
- use hierarchy instead of a border around every block;
- use tabular numerals for measurements;
- show timestamps and freshness;
- encode state with text, shape/icon, and color;
- design for operator viewing distance;
- avoid animation except for meaningful state changes.

#### Safety and accessibility

- Never communicate state by color alone.
- Make stale data visibly different from live data.
- Do not allow accidental entry into compose mode on an operator station.
- Confirm destructive or line-affecting operations.
- Define touch target and contrast requirements.

### 5. Visual system and component architecture

Plugin webviews use VS Code variables appropriately, but independently repeat
card spacing, borders, radii, inputs, muted colors, and status treatments. HMI
code also assigns substantial styling through `style.cssText`.

Shared raw colors are not a shared design language.

#### Recommendation

Create semantic tokens:

```css
:root {
  --xi-surface-canvas: ...;
  --xi-surface-panel: ...;
  --xi-surface-raised: ...;
  --xi-text-primary: ...;
  --xi-text-secondary: ...;
  --xi-border-subtle: ...;
  --xi-state-ok: ...;
  --xi-state-ng: ...;
  --xi-state-warning: ...;
  --xi-state-system: ...;
  --xi-space-1: ...;
  --xi-radius-control: ...;
}
```

Map these to VS Code variables in webviews and to standalone theme values in
the HMI. Build only the repeated primitives:

- `xi-panel`
- `xi-section-header`
- `xi-field`
- `xi-button`
- `xi-status`
- `xi-metric`
- `xi-empty-state`
- `xi-alert`

Do not attempt a general-purpose design system.

There are also two dashboard renderers: a compose-capable renderer in
`hmi/app.mjs` and a run renderer in
`ui-components/src/dashboard/dashboard.mjs`. Both implement tabs, recursive
nodes, sizing, and treatment. The shared library should own rendering and expose
composition hooks. The HMI should own connection, shell, persistence, and
production policy.

#### Acceptance criteria

- One renderer defines tabs and split behavior.
- Plugin UIs no longer copy base form and card CSS.
- A state color or spacing change has one intentional source.
- CI detects a stale committed HMI bundle.

### 6. API, vocabulary, and naming

The ABI policy, exact protocol reference, and documentation coverage checker are
strong examples of constraints replacing memory.

Historical vocabulary still remains near the active surface:

- `VAR` remains prominent while the overview says it no longer publishes;
- legacy tests remain in the active test inventory;
- the HMI README first describes vars/image preview, then says the HMI carries no
  vars/image decoding;
- FE/BE terminology is precise for maintainers but weak for operators.

#### Recommendation

Use vocabulary by audience:

| Audience | Preferred vocabulary |
|---|---|
| Inspection author | project, script, instance, parameter, input, output, result |
| Plugin author | plugin, record, image, schema, host interface |
| Operator | line, station, inspection, verdict, alarm, state |
| Maintainer | backend, FE, ABI, dispatch group, trigger bus |

For every deprecated concept:

1. mark it compatibility-only;
2. remove it from the first successful example;
3. move its tests into a compatibility section;
4. define removal criteria or permanent-support policy.

Version truth is also only partly centralized. The backend and extension are
`0.2.0`, while UI components and the Python client are `0.1.0`. Independent
versions may be correct, but the compatibility policy is unclear.

Choose one model:

- **Lockstep:** shipped artifacts share a release version and CI checks it.
- **Independent:** packages version separately with a compatibility matrix.

### 7. Documentation system

The documentation architecture is strong:

- guides, reference, and internals have different jobs;
- the one-home rule is explicit;
- public ABI and WS coverage is checked automatically;
- removed designs retain their rationale;
- known issues distinguish accepted trade-offs from real defects.

Coverage is not semantic consistency. `check_doc_coverage.py` proves that a
symbol is mentioned; it cannot detect stale architecture prose or contradictory
status claims.

The HMI README demonstrates this:

- the opening refers to vars and image previews;
- the data-model section says that path no longer exists in the HMI;
- the title says v1.0 RUN mode while scope says v1.1 compose mode is current.

#### Recommendation

1. Add `Status: shipped | experimental | planned | archived` to design docs.
2. Add `Last verified against:` to operational guides.
3. Add a forbidden-term check for removed concepts in active overview docs.
4. Test links and executable snippets.
5. Pair screenshots with the version or commit that generated them.
6. Review a complete user journey per release, not only symbol coverage.

Reorder the root README:

1. one-sentence value;
2. screenshot or result demonstration;
3. five-minute path;
4. core vocabulary;
5. architecture and principles;
6. advanced and contributor material.

### 8. Testing and quality strategy

The repository has unusually broad coverage: C++ units, protocol integration,
adversarial tests, stress tests, real processes, extension-host journeys,
screenshots, crash dumps, supervisor behavior, ABI freeze, and doc coverage.

The weakness is hierarchy. Many tests prove mechanisms, while the intended user
contract is distributed. Screenshot tests are human spot checks rather than
deterministic visual gates, and legacy suites remain beside active behavior.

#### Recommendation

Create a small release-blocking product-invariant suite:

1. A new project reaches first result.
2. A compile error explains the cause and preserves the last good runnable state.
3. A backend crash produces an unambiguous degraded or faulted UI.
4. A reconnect never displays stale success as live.
5. An AOT export runs without the development toolchain.
6. An operator can identify the last NG and timestamp.
7. Authenticated remote connection follows the documented default path.

For UI quality:

- deterministic screenshots for critical states;
- narrow, desktop, and operator-wall dimensions;
- supported VS Code light and dark hosts;
- accessibility checks for labels, contrast, keyboard order, and color-only
  status.

### 9. Operations and reliability

The project treats operational realities seriously: crash recovery, degraded
compile state, crash history, queue/drop metrics, AOT deployment, atomic writes,
and working-copy ownership.

These exist as mechanisms, but clients still need to synthesize the truth.
Status, logs, results, dispatch statistics, FE state, connection, and crash
history should produce one authoritative health model.

#### Recommendation

Define a canonical health contract containing:

- overall state;
- generation and timestamp;
- backend identity and version;
- project and compiled revision;
- last successful inspection timestamp;
- compile state;
- queue/drop condition;
- active faults;
- recovery recommendation.

Clients should not independently infer health semantics. Also define freshness
budgets for connection, results, status, and counters, including whether a
counter is run-, process-, or lifetime-scoped.

### 10. Security and deployment posture

Remote bearer authentication exists and has an end-to-end test for missing and
bad credentials plus constant-time comparison.

The convenient HMI deployment path recommends exposing a same-origin proxy via a
tunnel, but secure authentication and origin policy are not dominant in those
instructions. This is not a vulnerability claim. It is a product-taste issue:
the easiest production-like path should be the safest path.

#### Recommendation

- Bind backend and demo servers to loopback by default.
- Require an explicit flag for non-loopback exposure.
- Make authenticated `wss` the primary remote example.
- Forward credentials without putting secrets in URLs.
- Validate allowed WebSocket origins in the proxy.
- Add a restrictive content security policy and security headers.
- Document secret storage, rotation, and log redaction.
- Separate demo tunnel instructions from production deployment.
- Add a threat model covering plugins, project code, remote clients, and browsers.

#### Acceptance criteria

- The main deployment example does not create an unauthenticated remote control
  surface.
- Logs never print bearer secrets.
- The in-process plugin trust boundary is explicit.
- Demo and production modes are visibly distinct.

### 11. Repository and release coherence

The root contains runtime, extension, components, HMI, plugins, SDK, protocol
fixtures, examples, tools, tests, docs, release, and screenshots. These are all
valid, but the ownership and release boundaries are not obvious.

#### Recommendation

Explain the repository as four products:

1. **Runtime:** backend, FE, runner, protocol.
2. **Authoring:** VS Code extension and project SDK.
3. **Integration:** plugin SDK, components, and client libraries.
4. **Operation:** HMI and deployment tools.

For each top-level package state:

- purpose and owner;
- whether it ships;
- version model;
- build and test commands;
- generated outputs;
- compatibility boundary.

The HMI commits a vendored component bundle. This is practical, but CI should
build the package and fail if the committed bundle differs. Use the same pattern
for copied versions, templates, and generated fixtures.

### 12. Engineering economy and feature budget

The roadmap's explicit deletion history is a strength. Removed systems do not
remain silently half-supported.

The risk is that complexity is rebuilding at the edges. The project now contains
a development environment, standalone HMI and composer, plugin webviews, shared
components, Python client, remote mode, recording/replay, pipeline visualization,
image tools, and multiple production executables.

Each feature is reasonable. Together they risk turning a sharp inspection
framework into a general industrial application platform.

Before accepting a feature, require answers to:

1. Which canonical journey becomes materially better?
2. Why is this part of xInsp2 instead of a plugin or external client?
3. Which existing concept or surface can it replace?
4. Who owns its test and documentation long term?
5. Does it strengthen the "one C++ file to production inspection" promise?

Prefer changes that remove steps and concepts over changes that add a mode.

## Prioritized Roadmap

### P0: coherence and truth

1. Correct the HMI README's stale model and version statements.
2. Define the canonical workflow and three primary intents.
3. Classify active, compatibility-only, and removed vocabulary.
4. Decide package version policy.
5. Make secure remote deployment the default documented path.
6. Add generated-bundle and version freshness checks.

### P1: structural simplification

1. Extract feature modules from `extension.ts`.
2. Make `ui-components` the single dashboard renderer.
3. Introduce semantic tokens and minimal shared primitives.
4. Separate operator, diagnostic, quality, and compose HMI concerns.
5. Define one canonical health/state contract.

### P2: product and visual refinement

1. Redesign HMI around the operator state model.
2. Rework the README around first success.
3. Add loading, empty, stale, degraded, faulted, and disconnected treatments.
4. Add deterministic visual and accessibility checks.
5. Establish a precision-instrument visual identity.

### P3: expansion after consolidation

- richer dashboard composition;
- multi-viewer fan-out;
- plugin-shipped HMI modules;
- Linux portability;
- expanded interactive teach tools.

These should not outrank coherence of the current workflow.

## 30-Day Plan

### Week 1: establish truth

- fix stale HMI documentation;
- write the canonical workflow and vocabulary table;
- classify commands as primary, contextual, advanced, or repair;
- decide package version policy;
- write the short threat model.

### Week 2: remove duplication

- extract one feature from `extension.ts`;
- consolidate dashboard rendering behind one API;
- add bundle and version freshness checks;
- introduce semantic state tokens.

### Week 3: operator model

- define HMI states and freshness rules;
- add degraded, stale, disconnected, and faulted treatments;
- separate operation from diagnostics;
- validate with a real operator or controls engineer.

### Week 4: prove the product

- restructure the first-run README path;
- create a release-blocking first-result journey;
- add visual fixtures for critical HMI states;
- test authenticated remote deployment from a clean machine.

## Decision Checklist

### Product

- Does this make the primary inspection loop shorter or clearer?
- Is it primary, contextual, advanced, or repair-only?
- Can an existing command, mode, or concept be removed?

### Architecture

- Can this be a plugin?
- If it enters a core, what invariant makes that necessary?
- Is there one structural owner for the state?
- Is the rule enforced or merely documented?

### UI

- What user question does this surface answer?
- What is the single primary action?
- Are loading, empty, stale, degraded, faulted, and disconnected states defined?
- Is status understandable without color?
- Is implementation vocabulary leaking into the user model?

### Documentation

- Is there one authoritative home?
- Does the text describe shipped behavior or a plan?
- Can drift be detected automatically?
- Does the first example use the preferred API?

### Operations and security

- What happens after a crash, reconnect, or partial failure?
- How does a user know whether displayed data is fresh?
- Is the easiest deployment path also the safe path?
- Are trust boundaries and accepted risks explicit?

### Maintenance

- Who owns the feature, test, and documentation?
- Does this add another representation that must stay synchronized?
- Can the change be tested without booting the entire product?

## Final Judgment

xInsp2 does not lack taste. It has **concentrated taste in the compute
architecture** and weaker taste in the surrounding product system.

The next quality step is not more technical capability. It is making the current
capability read as one deliberate product:

- one canonical workflow;
- one vocabulary per audience;
- one owner for each state and renderer;
- one operator truth model;
- one safe deployment story;
- fewer equally prominent choices.

If that consolidation is done, the strongest technical decisions will become
visible to users instead of remaining qualities that only maintainers can see.
