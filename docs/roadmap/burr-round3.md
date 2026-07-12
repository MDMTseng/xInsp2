# Burr audit — round 3 (2026-07-12)

> **Status: EXECUTED.** One test fix + three waves landed on `audit/burr-round3`:
> lifecycle fix `1444026`, W1 `2b1d27c`, W2 `2fe26f6`, W3 = this commit. Where
> round 2 swept the backend for contraction burrs, round 3 turned the audit on
> **round 2 itself** (adversarial re-review of its own fixes), then swept the
> territories round 2 never entered, plus the deferred items recorded below
> with reasons.

## The three lenses

1. **Adversarial review of round 2's own fixes** — every round-2 change
   re-read assuming it introduced a regression. **Verdict: no BROKEN.** Six
   suspicious findings, all resolved: S1–S5 fixed in W1 (`2b1d27c` — the
   prepare/quarantine dead-end, the stale-module verdict, the warn_use_*
   build-then-check regression, the door-gate precedence, the `catch(...)`
   empty-tail stamp), and the sixth — the standing Debug-only
   `lifecycle_asserts` red — diagnosed and fixed in `1444026` (below). A
   seventh observation (S6, launch-pause past `send_rsp_ok`) was assessed
   benign and is recorded in Deferred.
2. **Fresh territories** — seven areas the previous rounds never audited,
   each with a verdict:
   - *Toolchain/process spawn* — FIXED (W1 #6): one bounded spawn primitive
     (`xi_proc.hpp`); the kill-only-cmd.exe orphan gap and both unbounded
     `std::system` win32 fallbacks are gone.
   - *KV state plane* — FIXED (W1 #7): `kKvHardCapBytes` enforced at the
     single host ingress; the last unbounded ingress edge is bounded.
   - *Crash/dump death path* — FIXED (W1 #8): the terminate/raise breadcrumb
     no longer allocates via `ctx()` on the death path (read-only
     scan-by-tid stamp).
   - *WS server* — FIXED (W2 handshake extractor + ct_equal + stop()
     on_close symmetry; W3 completed the busy-reject 503 drain portability —
     the non-blocking drain now runs on POSIX too, stale TODO(linux) gone).
   - *Hot mechanical residue (protocol/ImagePool/Param/proc quoting)* —
     FIXED (W2): dead `after_out` cursor, shared `lookup_`, `%.17g` param
     wire precision, one `quote_arg` primitive, `clear_script()` deleted.
   - *Plugin fleet drift* — FIXED (W3, this commit): the dead `pack_mode`
     knob deleted ×3 (json_source / mock_camera / synced_stereo — set but
     never read since THE CUT made packs the sole emit currency), decl.json +
     codegen regenerated, READMEs/examples cleaned; synced_stereo gained the
     schema-skew guard its siblings carry and lost its stale bilingual
     header; cache's always-true `Entry::is_pack()` + guards + interleave
     prose deleted; blob_analysis gained `set_max_area` (exchange symmetry
     with set_def) + the sibling fail-loud payload guard; base64 ×3
     consolidated into `xi/xi_b64.hpp`.
   - *Frozen-header prose* — FIXED (W3, comment-only; `test_abi_freeze`
     green): the v11-titled FROZEN SIGNATURE banner rewritten for v12 with
     assert-relative offsets, the `get_interface` id list regenerated from
     the real resolver branches (retired `xi.doc@1` annotated, `xi.emit@1`
     corrected to emit_binary-only, the pack/cap doors added), the v1
     SHM-history sentence corrected, the dead `xi_plugin_record_schema_fn`
     typedef deleted (no host GetProcAddress remains — silent false safety),
     and imgcodec's "host field stays untouched" claim fixed.
3. **`lifecycle_asserts` diagnosis** — the standing Debug-only red was
   FIXTURE DRIFT, not a product defect: the adapter enforces
   prepare/commit as a contract PAIR (exporting exactly one nulls both),
   and `reentry_probe` exported only commit, so the guard under test was
   unreachable. Fixed in `1444026` (a no-op prepare export); the Debug
   assert itself is correct and untouched. **The suite is now fully green
   in BOTH configs — the last standing red is gone.**

## The four commits

- **`1444026`** — test(lifecycle): the Debug-only `lifecycle_asserts` red;
  fixture drifted behind the prepare/commit pair rule. Debug 78/78.
- **W1 `2b1d27c`** — the adversarial-review regressions (S1–S5) + fresh
  territory: bounded toolchain spawn, the KV hard cap, the crash-path
  breadcrumb stamp.
- **W2 `2fe26f6`** — ritual/dedup wave: WS handshake extractor + ct_equal +
  stop() symmetry, protocol cursor deletion, ImagePool `lookup_`, the
  guarded_script_call KV capture/restore structure, `quote_arg`,
  `clear_script()` deletion, Param float precision, yyjson link probe.
- **W3 (this commit)** — frozen-header prose corrections (comment-only,
  ABI untouched), the plugin dead-knob/drift sweep, the `xi_b64.hpp` leaf,
  the portable WS busy-reject drain, and this document.

## Deferred — with reasons

- **Observability hand-rolled JSON → `xi::Json` adoption** — wave-sized;
  string-assembled JSON is an injection-risk *surface* by construction, but
  every current site was verified correct (escaped or provably-safe
  content), so the rewrite is a quality ratchet, not a fix. Do it as its
  own wave with the docs/wire gates watching.
- **ImagePool god-header split** (`xi_host_door.hpp` + `PublishedSlot<T>`) —
  wave-sized structural work; the header currently hosts the pool, the
  door registry, and the slot-bridge pattern in one file. Belongs to a
  structural round, not a burr sweep.
- **`stats(owner=0)` dual meaning** — owner 0 is both "no owner context"
  and "framework-transient"; disambiguating touches the ledger contract
  and its tests. Needs its own design pass.
- **VS2019 vcvars probe policy** — the compiler probe's version-selection
  policy is a toolchain-support decision (which VS generations to keep
  probing), not an audit cleanup. CT's call.
- **WS inbound triple-copy** — recv buffer → frame reassembly → callback
  string; fine for the cmd channel's message sizes, matters only if binary
  UPLOADS ever ship. Revisit with that feature, not before.
- **S6: launch-pause extends past `send_rsp_ok`** — the pause covers the
  reply send of the launching command; benign because commands are
  serialized on the WS lane (nothing else can interleave), so it only
  delays the ack by the pause tail. Noted so it's a decision, not an
  oversight.

## Round-2 deferred reconciliation (2026-07-12)

All nine items on [burr-round2.md](burr-round2.md)'s deferred list remain
deferred — none were in round 3's scope. (The `TODO(linux)` WS busy-drain
gap closed by W3 lived in code, not on that list.)
