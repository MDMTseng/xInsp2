# ADR-001 — Freeze `xi_host_api` (v9 baseline → **v11 baseline** after Phase 4)

**Status:** Accepted (Phase 0); **superseded baseline as of Phase 4 — now `xi_host_api@11`**
**Date:** 2026-07-01
**Context:** [`core_fix_plan.md`](core_fix_plan.md) §10–12 (host-API evolution: monolith → segregated, frozen capability interfaces). This ADR originally recorded **Phase 0** only — the freeze discipline and its safety nets, with **no ABI/layout change**. Phase 4 (see the addendum at the end) executed the authorized "retire the monolith" break and **moved the frozen baseline from v9 to v11**; the discipline below is unchanged and now applies to the v11 layout.

## Decision

1. **`xi_host_api` version 9 is frozen.** Its published layout — the field order and each field's exact function-pointer type, as defined in `backend/include/xi/xi_abi.h` — is fixed and will not be edited in place.

2. **A published `(interface, vN)` is frozen forever.** Any add, change, or remove of a host capability ships as a **new version** (the next monolith append → `v10`, and later as a per-capability interface `@N+1`), never as an in-place edit of the `v9` fields. This is the rule that lets a plugin compiled against `v9` keep running unchanged across every later phase (`core_fix_plan.md` §14, invariant 4).

3. **Two build-failing guards enforce the freeze.** This repo has **no GitHub Actions / CI runner**, so the "CI freeze guard" of the plan is realized as tests in the existing suite (`backend/CMakeLists.txt` / `ctest`), backed by compile-time `static_assert`s:

   - **Golden-plugin compatibility test** — `backend/tests/golden_plugin.cpp` + `backend/tests/test_golden_plugin.cpp`. A minimal but real C-ABI plugin pinned to ABI v9 / min-compat 6 is loaded through the genuine plugin-load path (`plugin_abi_compatible` → `CAbiInstanceAdapter`) and its `process()` is run once, asserting byte-for-byte stable behaviour (a 4×4×1 image with a deterministic pixel ramp). It is built **from source** against the live headers (not a committed binary blob), so it always tracks the current ABI while its asserted behaviour stays frozen. **This is the single most important safety net — do not touch the ABI without it.**

   - **ABI freeze-signature guard** — `backend/tests/test_abi_freeze.cpp`. Captures the canonical v9 signature: every field's **offset** and exact **function-pointer type**, in order, plus the size and version pins. Almost entirely `static_assert`, so any reorder, retype, insert, or remove of a `v9` field **fails the build**. This strengthens the seed guard that already lived in `xi_abi.h` (the `sizeof` + `offsetof(compress_image)` asserts) into a per-field freeze.

## How to evolve the ABI (so the guards stay green legitimately)

Do **not** edit the freeze table or the golden plugin to make a build pass. Instead, per `core_fix_plan.md` §12:

- **Phase 1** — append one field (`get_interface`) at the struct tail, bump `XI_ABI_VERSION` → 10, update `XI_ABI_EXPECTED_SIZE`, and add a **second** frozen table for v10. The v9 table stays as the permanent record; the v9 golden keeps running (now exercising min-compat) to prove old plugins still load.
- **Phase 2+** — carve frozen per-capability interfaces (`xi.preview@1`, `xi.imaging@1`, …), each with its own freeze table and, ideally, its own golden.

## Consequences

- **Positive:** an old plugin can never be silently broken by a layout edit; the break surfaces at build time (freeze guard) or first run (golden test). Adding capabilities becomes a deliberate, versioned act.
- **Cost:** the dead `shm_*` stubs cannot be deleted while v9 is published (removing them reshuffles offsets). They retire in Phase 4 when the break is taken (see addendum).
- **Scope of this ADR:** Phase 0 (freeze) through **Phase 4 (the authorized break)**; the addendum records the v11 rebaseline. `get_interface`, per-capability structs, etc. are Phase 1–3.

## Addendum — Phase 4: retire the monolith, rebaseline to v11 (2026-07-01)

**Status:** Accepted (user-authorized major ABI break — `docs/internals/OPEN-QUESTIONS.md`, "Decisions made").

Phase 4 (`core_fix_plan.md` §12) deliberately took the **one breaking cut** the freeze had been deferring. It is **not** additive:

1. **Removed the five dead `shm_*` stubs** from `xi_host_api` (always null since the 2026-05 process-isolation removal). Every field after `instance_folder` shifts down 5 pointers (40 bytes).
2. **Retired `get_interface("xi.legacy", 9)`** — the whole-table legacy view is no longer published (a "stop answering a query id" change). The carved interfaces (`xi.imaging` / `xi.doc` / `xi.emit` / `xi.log` / `xi.preview` @1) remain the capability path; the struct fields (minus `shm`) remain for direct callers.
3. **Bumped `XI_ABI_VERSION` 10 → 11** and **raised `XI_ABI_MIN_COMPAT` 6 → 11**: every pre-v11 plugin (compiled against the old `shm`-bearing layout) is now **refused** at load rather than handed a mis-offset table. Authorized because all first-party plugins are rebuildable in-tree.

**New layout:** `XI_ABI_EXPECTED_SIZE` = **176** (22 function pointers). `compress_image` @ 160, `get_interface` @ 168 (still the last field / query door).

**The v11 layout is the NEW frozen baseline — the freeze discipline resumes from here.** The two guards were re-snapshot to v11:
- `test_abi_freeze.cpp` pins the 22-field v11 signature (offsets + types), size 176, version 11, min-compat 11.
- `golden_plugin.cpp` is re-pinned to v11 (pinned to `XI_ABI_MIN_COMPAT`, the oldest still-loadable version); `test_golden_plugin.cpp` proves a current plugin loads **and** adds `stale_plugin` fixtures (reporting v9 and v10) asserting a pre-v11 plugin is now **REFUSED** — proving the break bites.

From v11 onward the original rule stands unchanged: a published layout is frozen; any add/change/remove ships as the next version, and new capabilities arrive as frozen per-interface structs behind `get_interface`, never as new `xi_host_api` fields.
