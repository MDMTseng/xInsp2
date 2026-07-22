# Plugin & Example Exemplar Quality Review

| Field | Value |
|-------|-------|
| **Date** | 2026-07-02 |
| **Reviewer** | Claude (external advisory) |
| **Status** | Advisory |
| **Scope** | The shipped `toolbox/`, the `qa/` scripts, and `sdk/templates` + `sdk/examples` — reviewed **as exemplars**: the de-facto teaching surface a plugin/script author copy-pastes from |

## Scope

This review judges the shipped artifacts by one question: **when a new author
copies this, do they learn the current, safe, blessed pattern — or a retired /
fragile / unsafe one?** It covers three surfaces, ranked by copy-likelihood:

1. **`toolbox/`** — the ~10 shipped plugins (`expose`, `blob_analysis`,
   `mock_camera`, `data_output`, `json_source`, `record_save`, `synced_stereo`,
   `config_swap_probe`, `cache`/buffer_replay, `record_load`). Checked for ABI
   v11 conformance, error handling at the C boundary, threading discipline,
   refcount/pool discipline, and JSON-parsing hygiene.
2. **`qa/`** — the user-script examples (`inspect.cpp` pipelines plus the
   top-level `defect_detection.cpp`, `use_demo.cpp`, `cancel_aware_script.cpp`,
   …). Checked for current-API usage (`XI_INSPECT_ENTRY` / `xi::use` / `xi::result`
   vs the retired `VAR` surface) and whether they compile.
3. **`sdk/templates` + `sdk/examples`** — the scaffold's canonical patterns.
   Checked for internal consistency: do template, plugin, and example teach the
   *same* style?

Ground truth was taken from the **current headers** (`backend/include/xi/*`), not
from prior reviews — `XI_ABI_VERSION` is **11** (`xi_abi.h:150`), and `VAR` is
**gone** (`xi.hpp:27`: "the old `VAR()`/value-store path was removed in v9";
`xi_script_support.hpp:123` confirms the value-store was removed). Every finding
below was re-verified against those headers.

This is a sibling to the already-triaged reviews. Where a finding overlaps a
triaged item it is referenced, not re-litigated. Review 02 P0 (the *generated
scaffold* taught the retired API) is **Bucket A**; this review asks the follow-on
the triage explicitly left open — *does `qa/` have the same rot?* It does,
at scale.

## Executive Summary

The exemplar surface is **bimodal**. There is one genuinely excellent,
internally-consistent teaching set — **`sdk/examples/`** (hello, counter, invert,
histogram, trigger_source, comm) — and every plugin author should be routed there
first. Against that high bar, the rest of the surface has three real problems a
copying author will hit:

- **The entire `qa/` *script* tree is compiled against a dead API.** 32
  `inspect.cpp` files carry **181** calls to the removed `VAR()` macro, including
  the README-advertised `defect_detection.cpp`. None of these compile on a current
  backend. This is the single highest-impact finding: the script examples are the
  first thing a new user runs, and they fail at the JIT step.
- **The two plugins a new author is *most* likely to copy teach a fragile
  pattern.** `mock_camera` (the walkthrough's `cam0`) and `blob_analysis` (the
  walkthrough's `det0`) both hand-roll JSON parsing with `cmd.find("\"key\"")` +
  `std::stoi` — precisely the anti-pattern the SDK's own README tells authors to
  avoid by using `xi::Json`. `blob_analysis` compounds it with a heap `xi::Image`
  (no `pool_image`) and a latent data race on a member string.
- **The three scaffold templates model three different base-class styles.**
  `easy` inherits `xi::Plugin` (modern); `medium` and `expert` do **not** — they
  re-declare `host_`/`name_` by hand and thereby forfeit `pool_image()`,
  `compress()`, `status()`, and the capability wrappers. `expert` additionally
  hand-rolls raw `host_->emit_record` refcounting. Which template you pick
  silently determines a different, sometimes worse, mental model.

None of these is a core-safety defect — the ABI boundary itself is well-defended
(`XI_PLUGIN_IMPL` catches every C++ exception per-call, `xi_abi.hpp:748-784`).
These are **teaching-quality** defects, and for an SDK whose thesis is "write a
plugin in 30 lines," teaching quality *is* the product.

The good news is that the fixes are cheap and mostly mechanical, and the correct
patterns already exist in-tree (`expose`, `config_swap_probe`, `sdk/examples/counter`)
to copy from.

## Scorecard

| Dimension | Grade | One-line rationale |
|-----------|:---:|---|
| ABI v11 conformance (plugins compile & load) | **B+** | All plugins build against v11 (DLLs present); boundary is exception-safe. Compiled `record_load` ships with no source. |
| `sdk/examples/` (plugin learning path) | **A−** | Clean, current, consistent — all inherit `xi::Plugin`, use `xi::Json`; `counter` is the canonical parse exemplar. The set to copy. |
| `qa/` script currency | **F** | 32 `inspect.cpp` (181 `VAR` uses) + `defect_detection.cpp` compile against the removed `VAR` API. |
| `sdk/templates` consistency | **C−** | Three templates, three base-class styles; `medium`/`expert` abandon `xi::Plugin` and its helpers. |
| Plugin JSON-parsing hygiene | **C** | `expose`/`record_save`/`config_swap_probe`/`json_source` do it right; the two most-copied (`mock_camera`, `blob_analysis`) + `data_output` hand-roll `cmd.find`+`std::stoi`. |
| Threading & refcount discipline | **B−** | Source workers follow the blessed `start/stop` (and `expert` uses `spawn_worker`); `blob_analysis` has a latent member-field race under parallel dispatch. |
| Docs/naming coherence | **C** | `cache` plugin is called `buffer_replay` everywhere in the docs; `sdk/README.md` still shows `VAR`; `record_load` has no source. |

## Findings

Ranked by what a copying author actually gets wrong first.

### 1. (P0) The `qa/` script tree teaches the removed `VAR()` API — nothing in it compiles

`VAR` was removed in v9. It is defined nowhere on the script compile path:
`xi.hpp:27` states it plainly, and `xi_script_support.hpp` (force-included into
every script DLL via `cl.exe /FI`) only carries a tombstone comment
(`xi_script_support.hpp:123`: "VAR value-store + image cache were removed"). A
script that uses `VAR` fails to compile with an undefined-identifier error at the
backend's JIT step.

Yet the script examples are saturated with it. A count over `qa/**/inspect.cpp`:

- **32 files, 181 `VAR(...)` occurrences.** The worst offenders:
  `golden_defect/inspect.cpp` (24), `io_stress/inspect.cpp` (21),
  `blob_tracker/inspect.cpp` (18), `circle_size_buckets/inspect.cpp` (13),
  `fixturing_demo/inspect.cpp` (13), `object_count_puzzle/inspect.cpp` (12),
  `circle_counting/inspect.cpp` (11).
- Plus top-level scripts named in the README repo map: `defect_detection.cpp`
  (`defect_detection.cpp:42-91`, 8 `VAR`s), and `cancel_aware_script.cpp`
  (`cancel_aware_script.cpp:42-71`).

Representative rot, `blob_tracker/inspect.cpp:121-197`:

```cpp
void xi_inspect_entry(int /*frame*/) {
    ...
    VAR(input,      frame);          // VAR removed in v9 — will not compile
    VAR(frame_path, fpath);
    ...
    VAR(crossings_so_far,  crossings_so_far_v);
}
```

Several of these files even *document* the removed macro's expansion in their
comments ("`VAR(name, expr)` expands to `auto name = expr;`",
`circle_counting/inspect.cpp:42`), which will actively mislead an author who reads
the example to learn the surfacing idiom.

**Consequence.** The script examples are the *first* artifact a new user exercises
(the README walkthrough §6-8 is "write `inspect.cpp` → compile → run"). They fail
at compile. Worse, an author who pattern-matches on them will write dead code and
conclude the framework is broken. `defect_detection.cpp` is called out by name in
the README's repo map, so this is a documented, linked, non-compiling example.

For contrast, the **correct** current idiom is already in-tree in two examples:
`parallel_inspect_demo/inspect.cpp:25` and `buffer_replay_demo/inspect.cpp` use
`XI_INSPECT_ENTRY(t, frame)` + `t.is_active()` + `xi::use("expose").process(rec)`
— exactly the README model. Only **2** of the ~50 script examples use the current
entry macro.

#### Recommendation

Treat this as the `qa/`-tree twin of the Bucket-A scaffold fix. Two tiers:

1. **Mechanical migration** of every `VAR(name, expr)` to
   `xi::use("expose").process(xi::Record().set("$channel", "name")…)` (numbers/strings)
   or `.image("name", img)` (images), and every `void xi_inspect_entry(int)` to
   `XI_INSPECT_ENTRY(t, frame)`. `parallel_inspect_demo` is the reference.
2. Or, if these examples are **historical** and not meant to compile, move them to
   `docs/archive/` and delete the README repo-map reference to
   `defect_detection`. Do not leave non-compiling scripts in the advertised
   `qa/` root. A one-line CI step (JIT-compile every `qa/**/inspect.cpp`
   against the current backend) would keep this from recurring — right now nothing
   compiles them (`qa/CMakeLists.txt` builds only the single M0 `demo_async`).

### 2. (P1) The two most-copied plugins teach hand-rolled JSON parsing the SDK's own docs warn against

The SDK README dedicates a section to `xi::Json` and states the case explicitly:
"a typical exchange handler shrinks from ~12 lines … to 3 lines … Reads on missing
or wrong-typed fields return the supplied default instead of crashing"
(`sdk/README.md`, `xi::Json cheatsheet`). Yet the two plugins the README
walkthrough puts in front of every new user do the opposite.

**`mock_camera`** — the walkthrough's `cam0` (README §2-3, first plugin, first
screenshot). `mock_camera.cpp:101-127`:

```cpp
std::string exchange(const std::string& cmd_json) override {
    if (cmd_json.find("\"start\"") != std::string::npos) { start_(); ... }
    if (cmd_json.find("\"set_fps\"") != std::string::npos) {
        auto pos = cmd_json.find("\"value\":");
        if (pos != std::string::npos) fps_ = std::stoi(cmd_json.substr(pos + 8)); // throws on junk
    }
```

and `set_def` (`mock_camera.cpp:87-99`) does the same substring-and-`std::stoi`
dance. A command like `{"command":"start_capture"}` matches the `"start"`
substring and fires the wrong action; `std::stoi` on a malformed value throws
(caught by the ABI wrapper, but it aborts the handler and silently rejects the
config).

**`blob_analysis`** — the walkthrough's `det0` (README §4). Same pattern,
`blob_analysis.cpp:171-187`: `cmd.find("\"set_threshold\"")` +
`std::stoi(cmd.substr(pos + 8))`, with the magic offset `+ 8` hard-coded to the
length of `"value":`. `data_output` (`data_output.cpp:25-55`) is a third instance.

`blob_analysis` has two further exemplar problems:

- **Heap image instead of `pool_image`.** `blob_analysis.cpp:129` allocates
  `xi::Image binary(w, h, 1)` on the heap, forcing `record_to_c` to allocate a
  pool slot and `memcpy` the pixels on the way out (`xi_abi.hpp:635-641`). The
  README's own 30-line plugin uses `pool_image(...)` for exactly this reason
  (zero-copy output), and the `easy` template's comments call it "the standard
  way for plugins to produce an output image." `det0` teaches the slow way.
- **Latent data race.** `cache_result()` writes the member `last_result_json_`
  during `process()` (`blob_analysis.cpp:190-192`) while `exchange()` reads it
  (`:183`). The SDK's own testing guidance says "the host may dispatch your plugin
  from parallel worker lanes" (`sdk/README.md`, testing section). Under
  reentrant/parallel dispatch, two `process()` calls writing a `std::string`
  concurrently is a race; there is no lock (compare `expose`, which guards all
  state with `mu_`). Config members (`thresh_`, `min_area_`, …) are likewise
  read in `process()` and written in `exchange()` with no synchronization.

**Consequence.** The plugins with the highest copy-probability model the fragile
parse, the non-pooled output, and unsynchronized shared state. A new author's
first plugin inherits all three, and the JSON one silently misbehaves on unexpected
input rather than failing loudly.

#### Recommendation

Rewrite `mock_camera`, `blob_analysis`, and `data_output` `exchange`/`set_def` in
`xi::Json` (they already link yyjson). Switch `blob_analysis`'s output to
`pool_image(w, h, 1)` and either lock its members or document it as
single-lane-only. These are the plugins new users see first — they should be the
best-lit, not the most-copied-and-wrong. `expose.cpp:94-159` is the in-tree model.

### 3. (P1) The three scaffold templates model three different authoring styles

`sdk/templates/` is described as "single source of truth for both VS Code + CLI"
(`sdk/README.md`). But the three templates disagree on the most basic decision —
whether a plugin *is* an `xi::Plugin`:

- **`easy`** (`easy/src/plugin.cpp:17`): `class {{CLASS}} : public xi::Plugin`,
  `using xi::Plugin::Plugin`, and comments that teach `pool_image()`,
  `as_cv_read()`/`as_cv_write()`. Modern and correct.
- **`medium`** (`medium/src/plugin.cpp:18`): `class {{CLASS}} {` — **does not
  inherit** `xi::Plugin`. It re-declares `host_`, `name_`, and a constructor by
  hand (`:20-23`, `:114-115`). It works only because `XI_PLUGIN_IMPL` duck-types
  on `host()`/`process()`/…, but the class has **no access** to `pool_image()`,
  `compress()`, `status()`, `folder_path()`, or the capability wrappers. It uses a
  heap `xi::Image bin(...)` (`:60`) instead of `pool_image`.
- **`expert`** (`expert/src/plugin.cpp:27`): also `class {{CLASS}} {` (no
  `xi::Plugin`). It correctly uses `xi::spawn_worker` for the SEH-safe worker
  thread (`:126`) — good — but then in `loop_()` it hand-rolls the raw host calls
  `host_->image_create` / `image_data` / `emit_record` / `image_release`
  (`:148-166`) with manual refcount balancing, exactly the refcount trap the SDK
  warns about, when the one-line `xi::emit_record(host, name, rec)` /
  `Plugin::emit(rec)` helper exists. Its `set_def` comment claims "Real plugins
  should use yyjson; this template avoids it to stay dependency-free"
  (`expert:48-49`) — but the same file `#include`s `yyjson.h` and uses it in
  `exchange()` (`:82`), so the justification is self-contradictory and the author
  is taught the fragile `std::stoi` path anyway (`:55`).

**Consequence.** The template an author picks silently decides their mental model.
Pick `easy` and you learn the blessed base; pick `medium`/`expert` and you learn a
hand-built plugin that can't reach half the SDK and juggles refcounts by hand. The
"single source of truth" teaches three sources.

#### Recommendation

Make all three inherit `xi::Plugin` and use `pool_image`/`xi::emit_record`/`xi::Json`.
`expert` should keep `spawn_worker` (its one genuinely good idea) but push frames
via `emit(rec)`, not raw `host_->emit_record`. The templates should differ in
*capability* (stateless → image-op → source), never in *style*.

### 4. (P2) `cache` plugin is named `buffer_replay` in every doc; `record_load` ships with no source

Two naming/provenance drifts that will confuse an author hunting for an exemplar:

- The record/replay reference plugin is implemented as `buffer_replay.cpp`
  (class `BufferReplay`) but its manifest names it **`cache`**
  (`toolbox/cache/plugin.json:2`, dll `xi-cache.dll`). Every doc calls it
  `buffer_replay`: README §Recording&replay ("The **buffer_replay** plugin…"),
  `sdk/README.md` ("the **buffer_replay** plugin captures…"), and the example is
  `examples/buffer_replay_demo/`. An author who reads the docs and looks for a
  `buffer_replay` plugin in the `+` picker finds `cache` instead (or nothing).
- **`record_load` ships as a compiled DLL only** —
  `plugins/record_load/xi-record_load.dll` with **no `.cpp` and no `plugin.json`**
  in the tree. A binary-only "shipped plugin" is not a teachable exemplar, can't be
  rebuilt against a future ABI, and is invisible to the review that the other
  plugins get. (It is also a minor supply-chain smell for a repo that otherwise
  ships source.)

Neither is a defect in the code that *is* there — `buffer_replay.cpp` is in fact
one of the better exemplars (see below). The problem is purely that the name the
author is told to look for and the name on disk disagree.

#### Recommendation

Rename the `cache` manifest to `buffer_replay` (or update all docs+example to say
`cache` — but the docs are more numerous, so rename the plugin). Restore
`record_load`'s source, or move its DLL out of `toolbox/` if it is a build
artifact rather than a shipped exemplar.

### 5. (Praise) The genuinely good exemplars — name these to authors

The correct patterns already exist in-tree. An author should be pointed at them
explicitly:

- **`sdk/examples/counter`** — *the* plugin to copy for the config + exchange
  loop. Its own header says so ("Uses `xi::Json` … the canonical way to parse
  commands and build replies", `counter.cpp:10-11`), and it ships a real native
  test (`counter/tests/test_counter.cpp`). The whole `sdk/examples/` set (hello,
  invert, histogram, trigger_source, comm) is clean, current, and uniformly
  inherits `xi::Plugin`.
- **`toolbox/expose`** — the best *shipped* plugin exemplar: `xi::Json` throughout,
  all state mutex-guarded (`expose.cpp:103`, `:124`, `:162`), images encoded via
  the SDK `compress()` wrapper with the correct `-needed`/retry convention
  (`:210-223`), and a clean self-describing binary frame format. This is the one
  to hold up as "what a good plugin looks like."
- **`toolbox/config_swap_probe`** — the reference for the ABI-v7 frame-perfect
  config swap: `std::atomic<std::shared_ptr<const Resource>>` double-slot,
  `prepare()` touches only the staging slot, `commit()` is a single atomic swap
  (`config_swap_probe.cpp:120-141`), opted in with `XI_PLUGIN_STAGED`. Exactly
  the contract the `xi_abi.hpp` comments describe.
- **`toolbox/record_save`** and **`cache`/buffer_replay** — both use `xi::Json`/
  proper yyjson, join their worker in the destructor, and (buffer_replay)
  demonstrate the correct "snapshot under lock, emit outside lock" discipline
  (`buffer_replay.cpp:81-107`).
- **Source-plugin threading** is broadly correct: `synced_stereo`, `mock_camera`,
  and `cache` run their capture/replay on a `std::thread` owned by `start_/stop_`
  and joined in the destructor — the blessed source pattern (SDK "Don't block"
  tip), not a raw thread on the per-frame path. (`synced_stereo`'s `fps_` is a
  plain `int` touched from two threads — a benign nit, worth making `std::atomic`
  for the example's sake.)

## Prioritized Roadmap

### Phase 1 — Stop teaching dead code (P0)
- Migrate or archive every `qa/**/inspect.cpp` using `VAR` (32 files) and
  the top-level `defect_detection.cpp` / `cancel_aware_script.cpp`. Use
  `parallel_inspect_demo` as the template.
- Add a CI job that JIT-compiles every advertised `qa/**/inspect.cpp`
  against the current backend, so this cannot regress.
- Fix `sdk/README.md:281-282` (and the `xi_use.hpp:143-150, 276-284` header
  comments) which still show `VAR(...)` / `void xi_inspect_entry(int)`.

### Phase 2 — Fix the most-copied plugins (P1)
- Rewrite `mock_camera`, `blob_analysis`, `data_output` `exchange`/`set_def` in
  `xi::Json`; switch `blob_analysis` to `pool_image` and lock/annotate its shared
  members.
- Unify `sdk/templates` `medium`/`expert` onto `xi::Plugin` + `pool_image` +
  `xi::emit_record` + `xi::Json`.

### Phase 3 — Coherence (P2)
- Rename the `cache` manifest to `buffer_replay` (or reconcile the docs).
- Restore `record_load` source or relocate its DLL.
- Add a one-paragraph "which exemplar do I copy?" table to `sdk/README.md`
  pointing at `counter` / `expose` / `config_swap_probe`.

## Decision Checklist

- [ ] Are the `qa/` scripts meant to compile? If yes → migrate off `VAR`.
      If no → move to `docs/archive/` and drop the README reference.
- [ ] Should the shipped plugins be held to the SDK's own `xi::Json` guidance?
      (Recommended yes — they are the walkthrough's first touchpoint.)
- [ ] Should all scaffold templates inherit `xi::Plugin`? (Recommended yes.)
- [ ] Is `record_load`'s binary-only shipment intentional? If not, restore source.
- [ ] Rename `cache` → `buffer_replay`, or accept the doc/name split and fix docs?

## Final Judgment

The **plugin ABI boundary and the `sdk/examples/` learning path are in good
shape** — the boundary is exception-safe by construction, and `sdk/examples/` is a
clean, consistent, current set that any author can safely copy. The framework
knows what a good exemplar looks like; `expose`, `config_swap_probe`, and
`counter` prove it.

The problem is that the **most-visible** artifacts — the `qa/` scripts a new
user runs first, and the `mock_camera`/`blob_analysis` plugins the README
walkthrough centers on — have drifted behind the API and the SDK's own stated best
practices. The `qa/` script rot (P0) is the one that will actually bite: it
does not compile, and it is linked from the README. None of these are hard fixes —
the correct patterns are all in-tree — but until they land, the teaching surface
tells a new author to write code the framework deleted a version ago. For an SDK
whose whole pitch is "copy this, write a plugin in 30 lines," that is the finding
that matters most.
