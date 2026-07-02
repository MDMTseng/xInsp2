# xInsp2 Core and Developer UX Review

| Field | Value |
|---|---|
| Review date | 2026-07-02 |
| Scope | Compute core, plugin developer UX, and inspection script UX |
| Status | Advisory review |

Related review: [`01-project-taste-review.md`](./01-project-taste-review.md)

## Scope

This review covers three connected layers:

1. **Compute core:** ABI, ownership, dispatch, lifecycle, protocol, health, and
   implementation boundaries.
2. **Plugin developer UX:** scaffold, manifest, build, test, reload, UI, export,
   and production readiness.
3. **Inspection script UX:** project creation, `inspect.cpp`, input, plugin calls,
   output, verdict, compile diagnostics, hot reload, and debugging.

The governing question is:

> Does the easiest developer path naturally produce code that respects the
> core's invariants and the project's current architecture?

At present, the answer is mixed. The underlying mechanisms are strong, but the
first-run templates and documentation still teach several retired concepts.

## Executive Summary

xInsp2's core has strong capability boundaries and substantial correctness
engineering, but runtime ownership remains concentrated in a few large
implementation units. Plugin and inspection UX lag behind the current
architecture because generated samples and onboarding still teach retired
concepts.

The highest-leverage change is:

> Fix every generated starting point before adding more API surface.

A developer will copy the scaffold and sample more often than they will read the
architecture documents. Templates are therefore part of the API.

## Scorecard

| Area | Score | Summary |
|---|---:|---|
| Core capability boundary | 8/10 | Domain knowledge stays out of the core |
| Core implementation boundary | 5/10 | Runtime responsibilities converge in large owners |
| Ownership model | 6/10 | Correctness is heavily tested, but images remain manually managed internally |
| ABI evolution | 7/10 | Strong guards and a capability door, with an overstated freeze story |
| Plugin API | 7/10 | Capable and fast, but the first useful plugin exposes too much machinery |
| Plugin first-run UX | 5/10 | Multiple paths and stale onboarding create conceptual load |
| Inspection API | 7/10 | Good primitives, but output and entry-point migration are unfinished |
| Inspection first-run UX | 3/10 | Generated sample teaches legacy entry and no-op `VAR` output |
| Diagnostics and recovery | 8/10 | Last-good reload and compiler feedback are strong |
| Overall developer coherence | 5/10 | Current implementation and taught model are not yet the same |


## Part I: Core Perspective

### 1. Minimal capability core versus minimal implementation core

The project's "minimal core" principle succeeds at the capability level:

- cameras are plugins;
- detectors are plugins;
- recording and replay are plugins;
- inspection output beyond verdict is a plugin;
- multi-camera gathering is a plugin;
- line integration belongs to a plugin and sidecar.

This is a real strength. The backend does not contain a hidden taxonomy of
machine-vision products.

Implementation-wise, however, the runtime core is not small:

- [`backend/src/service_main.cpp`](../../backend/src/service_main.cpp) is about
  5,000 lines;
- [`backend/include/xi/xi_plugin_manager.hpp`](../../backend/include/xi/xi_plugin_manager.hpp)
  is about 2,800 lines;
- command handling, script lifecycle, dispatch, status, results, watchdog,
  project state, and server concerns meet in the same process-global runtime.

The command table is an improvement over a giant conditional, but handler
extraction alone does not create subsystem boundaries. Most handlers still
operate directly on `g_eng`.

#### Recommendation

Keep one process and the current performance model, but divide runtime ownership:

```text
Engine
├── ProjectRuntime
│   ├── project model
│   ├── working copy
│   └── toolchain resolution
├── ScriptRuntime
│   ├── compile/load/swap
│   ├── state migration
│   └── params
├── PluginRuntime
│   ├── registry
│   ├── instances
│   └── rebuild/reload
├── DispatchRuntime
│   ├── ingress
│   ├── lanes
│   ├── ordering
│   └── watchdog
├── HealthRuntime
│   ├── status
│   ├── errors
│   ├── freshness
│   └── metrics
└── CommandRouter
```

This is not a request for service objects around every function. A boundary is
valuable when it owns state, establishes a lock order, and provides a small
operation set.

#### Acceptance criteria

- A command handler does not coordinate several runtime subsystems directly.
- Lock ordering belongs to runtime owners rather than handler comments.
- Project close and process shutdown call explicit owner teardown methods.
- A subsystem can be unit-tested without constructing the WebSocket server.

### 2. Runtime ownership is still implicit

`g_eng` consolidates many former globals, which improves visibility. It does not
fully own the runtime. Important state also lives in:

- `ImagePool` singleton;
- `DocRegistry` singleton;
- `TriggerBus` singleton;
- thread-local current trigger;
- thread-local staged emissions;
- thread-local run result;
- DLL-owned script and plugin state.

The code contains careful comments about static destruction, module lifetime,
release order, and controlled teardown. Those comments reveal real expertise,
but also reveal that the ownership graph is not represented structurally.

#### Recommendation

Create a `RuntimeContext` whose lifetime dominates project, plugin, script,
dispatch, image, and document state. Singletons can remain internally during
transition, but new code should receive explicit references from the context.

Move toward this order:

```text
construct context
  -> construct pools and registries
  -> construct project/plugin/script owners
  -> construct dispatch
  -> attach transport

detach transport
  -> quiesce dispatch
  -> release script/plugin modules
  -> release project state
  -> destroy registries and pools
```

The goal is not dependency injection as style. The goal is making illegal
teardown order difficult to express.

### 3. Image and document ownership are asymmetrical

Document ownership has moved toward typed RAII through `DocRef`. Image ownership
inside `TriggerEvent` still uses raw `xi_image_handle` values and manual
`ImagePool::addref/release`.

This creates a split model:

- metadata is move-owned and destructor-safe;
- images depend on release helpers and every early/drop path being correct.

The repository has already added RAII guards around risky paths. The next
coherent step is an internal move-only image reference.

#### Recommendation

Introduce an internal `ImageRef`:

```cpp
class ImageRef {
public:
    static ImageRef adopt(xi_image_handle);
    static ImageRef retain(xi_image_handle);

    ImageRef(ImageRef&&) noexcept;
    ImageRef& operator=(ImageRef&&) noexcept;
    ~ImageRef();

    xi_image_handle get() const;
    xi_image_handle release();
};
```

Use raw handles only at the C ABI boundary. `TriggerEvent` should own
`unordered_map<string, ImageRef>`. This does not require heap allocation beyond
what the current map already does and does not alter the ABI.

#### Acceptance criteria

- A moved or dropped `TriggerEvent` releases every image by construction.
- Release helpers no longer manually walk event images.
- ABI adapters explicitly choose `adopt` or `retain`.
- Refcount tests cover copy/retain, move, duplicate-key, queue drop, and shutdown.

### 4. Read-only input is not enforced

The host imaging interface returns a mutable `uint8_t*` from `image_data()`.
Inputs may alias the same pooled frame across consumers. A plugin that performs
an in-place OpenCV operation can therefore mutate another consumer's input.

The documented rule is read-only input by convention. The type system permits
the opposite.

This is the most important remaining core contract debt because it crosses
correctness, plugin UX, and concurrency.

#### Recommendation

Introduce a new imaging capability that distinguishes access:

```c
const uint8_t* image_read(xi_image_handle);
uint8_t* image_write(xi_image_handle);
```

`image_write` should only succeed for a newly created output or when the caller
has an explicitly guaranteed unique writable handle. Do not retrofit silent
copy-on-write into the hot path.

In C++ wrappers:

- `const Image&` exposes only a const view;
- output creation returns a writable image;
- an explicit advanced API handles unique mutable ownership.

#### Plugin UX consequence

The easiest sample must demonstrate:

```cpp
auto src = in.image("frame");       // read-only
auto dst = output_image_like(src);  // writable output
cv::threshold(as_cv(src), as_cv(dst), ...);
```

It must never normalize in-place mutation as the shortest example.

### 5. ABI policy is good but described too absolutely

The ABI has strong properties:

- a C boundary;
- layout assertions;
- minimum compatibility checks;
- capability interfaces behind `get_interface`;
- tests for compatibility and door-to-field equivalence.

Version 11 was nevertheless an intentional ABI break that removed dead fields
and raised `XI_ABI_MIN_COMPAT` to 11. This can be the right decision, but the
public story should not imply uninterrupted binary stability.

#### Recommendation

State the policy precisely:

- ABI v11 is the current frozen baseline.
- Pre-v11 plugins must be rebuilt.
- Existing v11 fields will not move.
- New capabilities use versioned interfaces behind `get_interface`.
- A future break requires a declared major epoch and migration plan.

Also make new SDK wrappers use carved interfaces by default. If first-party
plugins continue to call monolithic fields directly, the capability door will
remain an architectural promise rather than the normal path.

### 6. The host API remains broad

`xi_host_api` still exposes imaging, documents, emit, logging, preview/compression,
instance folders, and ownership services as one table. `get_interface` creates a
future escape path, but the old shape remains attractive because direct field
access is shorter.

#### Recommendation

Do not remove the v11 fields. Instead:

1. centralize capability lookup in SDK wrappers;
2. cache typed interface pointers per plugin instance;
3. make common plugin code independent of `xi_host_api` layout;
4. reserve direct table access for the bridge and advanced compatibility code.

The UX target is:

```cpp
images().create(...);
emit().record(...);
log().warning(...);
```

not:

```cpp
host_->get_interface(...);
```

The SDK should hide ABI mechanics without hiding cost or ownership.

### 7. `TriggerBus` carries retired terminology

The current header explicitly says there is no correlation and each emitted
record becomes one event. The type now acts as an ingress funnel with one sink.
The `TriggerBus` name still implies a richer routing and correlation model.

Comments in [`backend/include/xi/xi_abi.h`](../../backend/include/xi/xi_abi.h)
also retain older language about frames sharing a trigger ID being correlated,
while the current trigger-bus implementation says correlation was removed.

#### Recommendation

- Internally rename the concept to `InspectionIngress` or `DispatchIngress`.
- Keep `Trigger` only for the script-visible inspection input if that term still
  provides value.
- Correct active ABI comments so they describe the v11 contract, moving old
  behavior into the version-history section.

Names should teach the current model, not preserve every historical phase.

### 8. Command parsing is not uniformly typed

The project has protocol parsing helpers, but some handler behavior still relies
on searching raw JSON strings. For example, `compile_and_load` recognizes
`optimize` through textual variants of `"optimize":true`.

This accepts implementation formatting rather than the JSON value model.

#### Recommendation

- Define a typed args struct for every command.
- Parse and validate at the router boundary.
- Pass a validated command object to handlers.
- Return structured validation errors with field paths.

The command router should own:

```text
parse envelope -> find command -> parse args -> authorize -> execute -> serialize
```

Handlers should not search raw JSON.

### 9. Health has many correct fragments but no single truth

The core exposes:

- run result;
- sticky status;
- logs;
- recent errors;
- dispatch statistics;
- compile events;
- crash breadcrumbs;
- supervisor state.

Each solves a distinct problem. Clients still need to infer whether inspection
is currently trustworthy.

#### Recommendation

Define a canonical health snapshot with:

- `state`: starting, ready, running, degraded, stopped, faulted;
- generation and timestamp;
- process, project, script, and plugin versions;
- compile status;
- last accepted trigger and result timestamps;
- drop/queue condition;
- active faults;
- recovery recommendation.

Preserve detailed streams, but make health semantics owned by the runtime rather
than reconstructed by each UI.

### 10. Header-only implementation is overextended

Header-only code is appropriate for:

- author-facing wrappers;
- templates;
- small inline value types;
- ABI declarations.

It is less appropriate for the plugin manager, compiler, server, image pool, and
other runtime implementations that have large stateful behavior.

#### Recommendation

Separate:

```text
backend/include/xi/   public SDK and ABI headers
backend/src/core/     private runtime implementations
```

This reduces compile coupling, makes private boundaries visible, and prevents
runtime implementation details from becoming accidental SDK surface.

## Part II: Plugin Developer UX

### 1. Current journey

The repository currently presents several overlapping plugin paths:

1. Create an in-project plugin from the VS Code extension.
2. Scaffold a standalone plugin with `sdk/scaffold.mjs`.
3. Copy an SDK example into a project plugin.
4. Place source under a configured external plugin directory with
   `compile: true`.
5. Use a CMake-owned plugin for external libraries or CUDA.
6. Export an in-project plugin as a distributable binary.

All are technically defensible. The first-time author should not need to
understand all six.

### 2. The onboarding documents disagree

[`docs/guides/write-a-plugin.md`](../../docs/guides/write-a-plugin.md) describes the
current explicit declaration model:

- every project plugin is declared in `project.json`;
- there is no folder auto-discovery for project use;
- plugins are trusted;
- in-project authoring is recommended first.

[`sdk/GETTING_STARTED.md`](../../sdk/GETTING_STARTED.md) still includes older
guidance:

- certification is described as a prerequisite near the top;
- plugins are described as external by design;
- loading is explained as scanning a parent folder;
- several paths imply discovery rather than explicit project declaration.

Later sections partially contradict those claims. This makes the plugin model
look more complex and less stable than it is.

#### Recommendation

Make one document the five-minute path:

```text
Create project plugin
-> choose "Image processor"
-> name it
-> create instance
-> run native sample input
-> change code
-> save and see reload
-> run generated test
```

Move standalone repositories, CUDA, search roots, export, staging, reentrancy,
and ABI details into progressive advanced sections.

The SDK getting-started document should either:

- become the standalone-only path and say so in the title; or
- delegate to the canonical plugin guide instead of retelling architecture.

### 3. Template levels describe complexity, not intent

The extension offers Easy, Medium, and Expert templates. These labels make the
developer estimate their competence rather than choose the kind of plugin they
need.

#### Recommendation

Name templates by role:

- **Processor:** record/image in, record/image out;
- **Source:** owns a worker and emits records;
- **Sink/Integration:** consumes ordered results or communicates externally;
- **Control-only:** configuration and exchange, no per-frame image processing.

Advanced concepts can appear as options:

- custom UI;
- background worker;
- staged configuration;
- reentrant processing;
- custom CMake/external dependencies.

This aligns scaffold choice with architecture.

### 4. The easy template is tutorial-heavy but not success-oriented

The easy template explains every hook and generates `get_def`, `set_def`,
`process`, and `exchange`, even though a minimal processor may only need
`process`.

This teaches the entire plugin base class before producing a visible result.

#### Recommendation

The first template should be a complete useful processor in approximately
30-40 lines:

```cpp
class Threshold : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& in) override {
        auto src = in.image("frame");
        if (src.empty()) return xi::na("missing frame");

        auto dst = output_image(src.width(), src.height(), 1);
        cv::threshold(as_cv(src), as_cv(dst), 128, 255, cv::THRESH_BINARY);
        return xi::Record().image("binary", dst);
    }
};

XI_PLUGIN_IMPL(Threshold)
```

Only generate config or exchange hooks when the selected template needs them.
Comments should explain the two important invariants:

- input is borrowed and read-only;
- output must be host/pool-backed.

### 5. The medium template uses a different authoring style

The easy template derives from `xi::Plugin`. The medium template manually stores
`xi_host_api*` and provides convention-based methods without deriving from the
base. Both work through `XI_PLUGIN_IMPL`, but a learner sees two architectural
models before understanding why.

#### Recommendation

All default templates should use the same high-level SDK style. Keep a separate
**Raw ABI / advanced integration** example for authors who need direct host
access.

The progression should be:

```text
Plugin base class
-> optional typed config helper
-> optional source/sink helpers
-> advanced raw ABI
```

not Easy API versus Medium manual host API.

### 6. Configuration requires too much JSON plumbing

Plugin authors repeatedly implement:

- `get_def()` serialization;
- `set_def()` parsing;
- numeric clamping;
- UI control synchronization;
- exchange command parsing.

The project already explores control descriptors and auto-panels. This is the
highest-value plugin SDK abstraction because it removes boilerplate without
entering the hot path.

#### Recommendation

Provide typed configuration descriptors:

```cpp
xi::Config config{
    xi::number("threshold", threshold, 128, 0, 255),
    xi::toggle("invert", invert, false),
};
```

The SDK can derive:

- get/set definition;
- validation;
- persistence shape;
- default auto-panel;
- manifest/input hints;
- test fixtures.

Custom UI remains an escape hatch. Configuration should be declarative;
per-frame processing remains ordinary C++.

### 7. Build and reload models are too visible

Authors encounter:

- direct `cl.exe` compilation for in-project source;
- CMake rebuild for external libraries;
- prebuilt DLL registration;
- hot save reload;
- explicit Rebuild Plugins;
- fixed-name Windows DLL replacement constraints.

These are real implementation distinctions, but the UI can describe outcomes:

- **Managed source:** save rebuilds automatically.
- **Custom build:** use Rebuild after source changes.
- **Binary plugin:** replace through its package/release process.

#### Recommendation

Show one plugin status row:

```text
Threshold Processor
Source: managed
Build: current
Loaded: v42
Instances: 2
```

When stale or failed, state exactly:

- which binary remains active;
- whether instances kept their state;
- which command repairs the condition;
- whether production is degraded.

### 8. Plugin test UX is strong but fragmented

Native tests and UI E2E scaffolding are a major strength. The developer must
still learn separate commands and understand cold versus warm sessions.

#### Recommendation

Expose one `Test Plugin` action with stages:

1. manifest validation;
2. ABI/load smoke test;
3. native unit test;
4. generated process-contract test;
5. UI test if present.

Return one report linked to source and artifacts. Keep individual commands for
CI and advanced debugging.

Generate contract tests from descriptors where possible:

- config round trip;
- missing input behavior;
- declared output shape;
- non-mutating input;
- reload state preservation;
- reentrancy policy.

### 9. Plugin production readiness is not summarized

The caveats guide contains important requirements, but authors must assemble the
shipping checklist themselves.

#### Recommendation

Add `xInsp2: Validate Plugin for Release`:

- manifest/schema valid;
- ABI version compatible;
- dependency DLLs resolvable;
- no absolute development paths;
- teardown succeeds;
- worker threads stop;
- generated tests pass;
- UI CSP and message validation pass;
- debug/Release distinction visible;
- PDB/package contents reported.

This should produce a machine-readable report used by export and CI.

## Part III: `inspect.cpp` Developer UX

### 1. The generated sample teaches the retired model

The sample project created in
[`vscode-extension/src/extension.ts`](../../vscode-extension/src/extension.ts)
currently generates:

- `XI_SCRIPT_EXPORT` and `xi_inspect_entry(int)`;
- ambient `xi::current_trigger()`;
- several `VAR(...)` calls;
- no `xi::result`, `xi::ok`, or `xi::ng`;
- no `expose` instance or official output call.

The current script guide says:

- `XI_INSPECT_ENTRY(t, frame)` is preferred;
- `VAR` and `EMIT` are legacy no-ops;
- `expose` is the official observational output;
- one result should represent the run verdict.

This is the most severe inspection-author UX issue. The generated first project
teaches APIs that the same documentation tells users not to use.

#### Recommendation

Change the generated sample before any other inspection UX work.

The sample should:

```cpp
#include <xi/xi.hpp>
#include <xi/xi_cv.hpp>
#include <xi/xi_result.hpp>
#include <xi/xi_use.hpp>

XI_INSPECT_ENTRY(t, frame) {
    auto input = t.image("frame");
    if (input.empty()) {
        xi::na("missing frame");
        return;
    }

    auto out = xi::use("detector").process(
        xi::Record().image("frame", input));

    int count = out["count"].as_int(0);

    xi::use("expose").process(
        xi::Record()
            .set("$channel", "inspection")
            .image("input", input)
            .image("binary", out.image("binary"))
            .set("count", count));

    count <= 3 ? xi::ok(1, "within limit")
               : xi::ng(1, "too many objects");
}
```

The project generator must also create or configure the required source,
detector, and expose instances so the sample works without hidden setup.

### 2. The script guide starts with the legacy form

The first large example in `write-a-script.md` uses the legacy entry and `VAR`,
then later explains that both are legacy. Even when the warning is accurate, the
reader has already encoded the wrong pattern.

#### Recommendation

Documentation order must be:

1. current minimal script;
2. input model;
3. plugin calls;
4. output and result;
5. params and state;
6. errors and hot reload;
7. compatibility appendix for legacy entry and `VAR`.

Legacy syntax should not appear in code that readers are invited to copy.

### 3. The umbrella-header story is inconsistent

Current `xi.hpp` comments say it is OpenCV-free and scripts opt in with
`xi_cv.hpp`. The generated sample comments that `xi.hpp` pulls in OpenCV. The
script guide also describes `xi.hpp` as including OpenCV in its first example.

#### Recommendation

Adopt one visible include rule:

```cpp
#include <xi/xi.hpp>         // core script types
#include <xi/xi_use.hpp>     // host instances
#include <xi/xi_cv.hpp>      // only when using OpenCV
#include <xi/xi_result.hpp>  // verdict helpers
```

If nearly every inspection needs `use` and `result`, consider a
`xi/inspect.hpp` authoring umbrella that includes the preferred script surface
without pulling plugin-author or runtime internals.

### 4. Too many output concepts remain visible

An inspection author encounters:

- `VAR`;
- `EMIT`;
- `expose`;
- `xi::result`;
- `xi::status`;
- logs;
- binary frames in lower-level docs.

The script guide now calls out two blessed output surfaces plus status, which is
the correct direction. The SDK and examples do not consistently reinforce it.

#### Recommendation

Use three verbs:

- **Judge:** `xi::ok`, `xi::ng`, `xi::na`;
- **Observe:** `xi::observe(...)` or a thin wrapper over the expose plugin;
- **Report state:** `xi::status(...)`.

The current `xi::use("expose").process(record)` is architecturally pure but poor
author UX. It exposes:

- a plugin instance name;
- the special `"$channel"` field;
- record construction;
- process semantics;
- the requirement that an expose instance exists.

Keep expose as the implementation. Add a script-side convenience wrapper:

```cpp
xi::observe("inspection")
    .image("input", input)
    .image("binary", binary)
    .value("count", count);
```

The wrapper should resolve a configured expose capability, fail with a precise
diagnostic when unavailable, and remain zero-copy.

This is ergonomic sugar justified outside the compute hot operation: it removes
four concepts from every inspection script without putting domain logic in core.

### 5. Input acquisition has competing paths

Scripts can receive:

- a pushed source record through `Trigger`;
- a `frame_path` from one-shot run;
- synthetic timer ticks;
- injected records in tests/replay.

The current examples often contain fallback behavior that manufactures an image
when no trigger is active. That is useful for demos but obscures production
semantics.

#### Recommendation

Make input mode explicit in project/sample templates:

- **Live source project:** `t.image("frame")`;
- **File tuning project:** `xi::input_image()` or an explicit file-run helper;
- **Source-less periodic script:** explicit timer entry/project flag;
- **Test injection:** test API, not production fallback code.

A missing required input should produce an NA/fault with a helpful message, not
silently substitute a synthetic image in a normal template.

### 6. Plugin names are stringly typed

`xi::use("detector0")`, record field names, channel names, and output keys are
all strings. Runtime flexibility is valuable, but typos become late failures.

The extension already knows instance definitions and plugin schemas.

#### Recommendation

Generate optional project bindings:

```cpp
#include "xinsp_project.hpp"

auto out = project::detector.process(
    project::detector::Input{}.frame(input));

int count = project::detector::Output(out).count();
```

Do not replace generic `Record`. Provide generated typed views for discovery,
completion, and compile-time checks while keeping the underlying ABI dynamic.

A lighter first step:

- completion for known instance names in `xi::use`;
- hover showing input/output schema;
- diagnostics for unresolved literal names before run;
- completion for known field names after a literal instance call.

### 7. Error behavior is powerful but not summarized in the editor

Compile errors produce squiggles and Problems entries. Failed compile preserves
the last good script and sets a degraded marker. This is excellent.

The developer still needs a compact answer to:

- Did my new code load?
- Is the old code still running?
- Which script generation produced this result?
- Did state migrate?
- Is continuous execution paused or resumed?

#### Recommendation

Add a script generation status:

```text
Inspection script
Generation 42
Source: saved 10:31:08
Compile: failed at 10:31:09
Active: generation 41
Run mode: resumed on last-good
State: preserved
```

Every run result should carry the active script generation. This prevents the
common developer mistake of believing a visible result came from the code that
currently exists in the editor.

### 8. Hot reload state rules are correct but cognitively expensive

Authors must understand:

- `xi::Param` persists;
- `xi::state()` persists;
- plain statics/globals are reinitialized;
- plugin instance state survives through host replay;
- schema migration may be needed;
- in-flight runs retain old DLL lifetime.

#### Recommendation

Provide explicit lifecycle hooks or a state declaration API that makes intent
visible:

```cpp
xi::State<MyState> state{"tracker", version<2>};
```

The current JSON state mechanism can remain underneath. The author should receive:

- schema mismatch diagnostics;
- migration examples generated with the project;
- a reload report listing restored params/state;
- a warning for likely mutable file-scope state not registered with xInsp2.

The last item can be heuristic and informational.

### 9. Inspection testing should be a first-class workflow

The repository has extensive backend and example tests, but an ordinary project
author needs a simple way to assert inspection behavior.

#### Recommendation

Support project-local inspection cases:

```yaml
cases:
  - image: frames/good_01.png
    expect:
      verdict: ok
      count: 2
  - image: frames/bad_01.png
    expect:
      verdict: ng
      class: 1
```

Expose:

- Run Current Case;
- Run Project Cases;
- Update approved observation snapshot;
- compare result, observed values, images, timing, and script generation.

Tests should use the same run path as production, not a separate inspection
implementation.

### 10. The pipeline graph still references `VAR`

Extension comments and examples continue to describe `VAR` chips in the graph.
If `VAR` no longer publishes and expose is the supported observation path, graph
semantics should follow actual records and plugin calls rather than legacy source
macros.

#### Recommendation

- Treat `xi::use` calls as stages.
- Use plugin schemas for declared edges.
- Use captured runtime provenance for actual edges.
- Represent observations as outputs/channels, not `VAR` declarations.
- Keep ordinary C++ local variables out of the graph unless they cross a stage
  boundary.

This preserves the principle that the script is source of truth without making a
dead macro part of the current mental model.

## Unified Developer Journey

### Desired inspection-author journey

```text
Create project
-> choose Live Camera or File Tuning
-> see generated current-style inspect.cpp
-> Run
-> receive verdict and observation panel
-> add an instance through completion/Quick Pick
-> save and see generation status
-> add project cases
-> export production bundle
```

The user should learn only:

1. `XI_INSPECT_ENTRY`;
2. `t.image`;
3. `xi::use`;
4. `xi::observe`;
5. `xi::ok/ng/na`;
6. `xi::Param`;
7. `xi::state` when needed.

Everything else is advanced.

### Desired plugin-author journey

```text
New Plugin
-> choose Processor, Source, Sink, or Control
-> generated useful implementation + test
-> create an instance
-> run against fixture
-> save/rebuild
-> inspect load generation and state restoration
-> validate for release
-> export/package
```

The user should learn only:

1. plugin role;
2. typed input and output;
3. read-only input/writable output;
4. typed config;
5. process or emit;
6. lifecycle/reentrancy when the chosen role requires it.

Raw ABI, search roots, loader behavior, and custom CMake come later.

## Prioritized Roadmap

### P0: stop teaching retired APIs

1. Replace the generated sample's legacy entry with `XI_INSPECT_ENTRY`.
2. Remove `VAR` from generated projects and preferred documentation examples.
3. Add a result and official observation output to the generated sample.
4. Create/configure the required expose instance automatically.
5. Correct the `xi.hpp`/OpenCV include descriptions.
6. Reconcile `sdk/GETTING_STARTED.md` with explicit declarations and trusted load.
7. Correct active ABI trigger comments that still describe removed correlation.

These are documentation/template changes with disproportionate UX value.

### P1: reduce concepts in the happy path

1. Add `xi::observe` as a script wrapper over expose.
2. Rename plugin templates by role.
3. Use one high-level plugin base style across default templates.
4. Generate only hooks required by the selected role.
5. Add script/plugin generation status to the extension.
6. Separate active tests from legacy compatibility tests in documentation.

### P2: encode invariants

1. Add internal `ImageRef`.
2. Add read-only/writable imaging capability separation.
3. Add typed command parsing.
4. Add typed plugin configuration descriptors.
5. Define the canonical health snapshot.
6. Generate project binding hints or typed views from schemas.

### P3: restructure without changing behavior

1. Split runtime ownership out of `service_main.cpp`.
2. Move private runtime implementation out of public headers.
3. Consolidate lifecycle and lock-order rules in runtime owners.
4. Rename internal trigger bus terminology.
5. Make carved capability wrappers the default SDK path.

### P4: complete the authoring system

1. Project-local inspection cases.
2. Unified plugin test and release validation.
3. Runtime-provenance pipeline graph.
4. State migration assistance.
5. Compatibility and package version matrix.

## Success Metrics

### Inspection author

- Time from project creation to first verdict.
- Number of concepts encountered before first result.
- Percentage of new scripts using `XI_INSPECT_ENTRY`.
- Percentage of new scripts with an explicit verdict.
- Number of generated or copied scripts containing `VAR`.
- Compile-failure recovery comprehension in user testing.

### Plugin author

- Time from scaffold to first loaded instance.
- Number of manually implemented config serialization lines.
- Reload failures with unclear active binary generation.
- Percentage of templates using the preferred high-level SDK.
- Release validation pass rate.
- Input mutation violations detected by tests.

### Core

- Manual image release sites.
- Direct `g_eng` access sites outside runtime owners.
- Raw JSON searches in command handlers.
- Runtime implementation lines in public headers.
- Direct monolithic `xi_host_api` field use in first-party plugins.
- Client-specific health inference logic.

## Final Judgment

The core has good architectural instincts and substantial correctness work. Its
main problem is not excess domain functionality; it is that ownership and
lifecycle complexity remain concentrated in a few large implementation units.

Plugin and inspection UX suffer from a different issue: the repository's current
architecture has advanced faster than its generated starting points. The system
supports a cleaner model than the one it teaches.

The correct sequence is:

1. make templates and first examples tell the truth;
2. remove unnecessary concepts from the happy path;
3. encode ownership and mutability invariants in types;
4. then split large runtime owners.

That sequence improves developer behavior immediately without destabilizing the
core that already works.
