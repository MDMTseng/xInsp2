# 20 — v12 execution checklist (THE CUT, cut-day worksheet)

Status: **worksheet** (drafted 2026-07-04). The step-by-step for the day THE CUT
is executed. doc 06 is the app-team brief (what/why/effort); doc 10 is the scope
decision; THIS doc is the operator's ordered runbook — what to delete, in what
order, what verifies each step, and where the one rollback point is.

**THE CUT is ONE commit** (or one tight commit series landing together) so there
is exactly one revert point. Everything below happens on a single `polaris2_main`
cut branch, gate-green, then merged as one event coordinated with the app team.

## 0. Preconditions — ALL must be true before cut day

- [ ] Gates P1/P2/P3 green (✅ done), U1/U2/U3 resolved (✅ done).
- [ ] Pre-cut prep landed: decode-eviction delegating slot (✅ done), **encoder
      eviction / core turbojpeg-free (V1)** — turbojpeg lives in imgcodec, core
      builds without it. **machine-autoload (V3)** if it's riding this train.
- [ ] App team: §6 answered (✅ all five), their plugins/scripts ported + soaked
      on `feat/pack-door-v12`, a **train date agreed**, a **rig + contact window**
      booked for the joint validation pass.
- [ ] Pre-cut `master` (or the current shipping tag) tagged as the revert anchor;
      `compat-manifest.json` names the pre-cut backend/ABI/client set.

## 1. Ordered sequence — port BEFORE delete (the dependency spine)

The order matters: you cannot delete Record while anything still consumes it, and
the examples tree is the Record path's regression net until the moment it dies.

**Stage A — port the last consumers (Record still present, bilingual)**
- [x] **Examples tree**: port all `inspect.cpp` off Record (~117 files use
      `xi::Record`) to the pack idioms (doc 06 §4.1 table). Examples that only
      existed to teach a Record pattern with no pack analogue: delete. Retire the
      Record-era QA that exist SOLELY as Record regression guards (they lose their
      job the moment Record is gone). Net: `run_qa` is pack-only after this stage.
- [x] **In-tree plugins**: the 19 plugin files using `xi::Record` — confirm each
      is bilingual (has its `xi.pack@1` door) and drop the Record `process()`
      override. (Shipped plugins are already bilingual — P1; this removes the now-
      dead Record half.)
- [x] App-team plugins/scripts recompile against the v12 SDK on their side (their
      port is done; this is their recompile-and-revalidate).
- [x] GATE after Stage A: full 8-stage gate green with Record still present but
      no in-tree consumer. This is the "safe to cut" checkpoint.

**Stage B — the deletions (the actual break, one commit)**
- [x] **ABI v12** (EXECUTED 2026-07-04, `polaris2/the-cut-v12`): `XI_ABI_VERSION 11 → 12`, `XI_ABI_MIN_COMPAT → 12`
      (`backend/include/xi/xi_abi.h:150/156`). Delete `xi_plugin_process_fn` +
      the `xi_record`/`xi_record_out` process path (`xi_abi.h:912` + the
      monolith struct fields). Plugin data plane = `xi.pack@1` door only.
- [x] **Formalize the capability plane** (RESOLVED: the zero-slot `get_interface` route stays; no `register_capability` slot. Net v12 ABI bill: −8 slots — read_image_file + emit_record + the six doc slots) as documented v12 ABI (`xi.cap` /
      `xi.cap.provider`). Decide: keep the zero-slot `get_interface` route
      (pilot) or add a `register_capability` host_api slot. (Pilot needs zero
      slots — net ABI bill is already −1 with read_image_file gone.)
- [x] **Delete Record + its machinery**: `xi_record.hpp`, `xi_doc_pool.hpp`,
      `xi_doc_registry.hpp`; the DocRegistry / COW / `share_out` / `adopt_shared`
      touchpoints in `xi_abi.hpp`, `xi_image_pool.hpp`, `xi_trigger_bus.hpp`,
      `xi_use.hpp`; the crash-leak counter (`crash_leaked_docs_lifetime`) made
      obsolete with it.
- [x] **Delete core image codec**: the `read_image_file` host_api slot + core's
      built-in stb DECODE fallback (`xi_image_io.cpp`); the in-core JPEG ENCODER
      (`xi_jpeg.hpp` / `compress_sink` / `XINSP2_HAS_TURBOJPEG` on the backend
      target). imgcodec (`xi.image.decode` / `xi.jpeg.encode`) is the only engine.
- [x] **`xi::state()` delete**: remove the Record-returning state API + its
      `xi_script_get_state`/`set_state` (JSON) exports; `xi::kv()` is the only
      state channel. (Scripts self-seeded during the window — doc 16.)
- [x] **Wire/format defaults**: XEX1-v3 becomes the default preview wire; XEX1-v1
      encode retained behind a flag for one release (delete next train).
      `record_save`'s `.json`+`.bmp` export door deleted.
- [x] **hello.abi 1 → 2**: `service_cmd_lifecycle.cpp:31` and
      `service_main.cpp:217` (both `"abi":1` → `"abi":2`). Clients compare + warn.
- [x] **Command retirements** (app team confirmed all five): delete
      `watchdog_status`, `unload_script`, `unquarantine_plugin`, `load_plugin`,
      `get_project` from `g_cmd_table` + their handlers.

**Stage C — sweep**
- [x] Grep the tree for every deleted symbol; zero references survive
      (retired-terms doc gate will enforce the vocabulary; add the v12-deleted
      names to it).
- [ ] Contract/fixtures: XEX1-v1 goldens move to a legacy-only fixture set or
      drop with the flag; v3 is the baseline.

## 2. Verification at the cut

- [ ] Full 8-stage `tools/gate.py` green on the cut branch (IN PROGRESS on `polaris2/the-cut-v12`) (docs/build/sdk/ctest/
      fixtures/live/qa/fuzz). The build stage alone proves no dangling Record
      reference compiles; qa proves every pattern still runs pack-only.
- [x] `test_abi_freeze` re-baselined to the v12 `xi_host_api` layout (the slot
      deletions shift offsets — this is an INTENTIONAL freeze-break, update the
      guard).
- [ ] Joint validation pass with the app team on the booked rig: their real
      projects, their real cameras, on the v12 backend.

## 3. Rollback

- One revert point: the cut is one commit/merge; `git revert` restores v11.
- The wire has a flag: XEX1-v1 is selectable for one release (display-wire
  rollback = config, not redeploy).
- The real safety net is the WINDOW: app-team code ported during the window runs
  on BOTH sides, so rolling the backend back does not roll their porting back.
  The only expensive rollback is un-porting — which is why porting + soak happen
  BEFORE the train and the train itself is a recompile + revalidation.

## 4. What this checklist is NOT

Not a licence to start deleting today. Stage A (porting) can proceed incrementally
pre-cut; **Stage B lands only on the agreed train date with the app team in the
loop.** Until then, no deprecation language anywhere (doc 10's rule) — a
half-deleted API teaches worse than either state.
