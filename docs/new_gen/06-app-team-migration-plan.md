# THE CUT — Cutover-Train Brief for the App Team

| Field | Value |
|---|---|
| **Date** | 2026-07-03 |
| **Audience** | The app-dev team consuming the xInsp2 backend: your inspection scripts, your plugins, your WS clients/tooling, your recorded artifacts |
| **Scope** | ONE coordinated break ("THE CUT"): plugin ABI v11 → v12 pure pack door, Record deleted, XEX1-v3 default wire, `hello.abi` 1 → 2, plus the retirements listed in §1 — and the bilingual window you can use TODAY to make the cut a non-event |
| **Status** | All engineering gates ACHIEVED on our side, and **§6 is ANSWERED (2026-07-04)** — all five decisions closed, app-side porting done + soaked (see §6's resolution block). THE CUT is GO pending only a train date on our release rhythm; two enablement items (E1 deployment refresh, E2 jpeg preview) tracked in §6 |
| **Supersedes** | The previous version of this document (the `polaris_master` merge brief). That migration completed; the `hello.abi` bump it deferred as "Phase 3" now rides this train |

## TL;DR

xInsp2's data plane has been rebuilt around **Pack**: one sealed, keyed, typed
container (canonical msgpack) that is byte-identical in memory, on the WS wire
(XEX1-v3), and on disk (`.xex1`). Every shipped plugin and every script surface
is already **bilingual** — the old `xi::Record` world and the new Pack world run
side by side in today's builds, proven by live QA. At THE CUT, the Record half
is deleted in one coordinated event shared with you.

What that means for you, compressed:

- **Nothing forces you to wait.** Every Pack surface (§3) works against the
  current backend. Port your scripts and plugins during the bilingual window,
  soak them under your own regression, and THE CUT becomes a recompile.
- **The porting is mechanical.** §4 is a line-for-line idioms table; typical
  script ports are minutes-to-an-hour each.
- **Nothing durable is at risk.** Recorded `.xex1` files are the format that
  survives; state is in-memory by design; project files don't change.
- **We need five decisions from you** (§6) — most importantly the train date
  and your confirmation on five candidate command retirements.

## §1 What changes at THE CUT — item by item

Effort classes: **S** = under an hour per affected unit · **M** = hours to ~a
day per unit · **L** = multiple days. "Zero" = no action if the "who is
affected" column doesn't describe you.

| # | Change | What breaks | What you do | Effort |
|---|---|---|---|---|
| 1 | **Plugin ABI v11 → v12: pure pack door.** `xi_plugin_process(Record)` and the Record-based monolith struct are deleted; a plugin's data plane is the `xi.pack@1` door only. The capability-plane interfaces (`xi.cap` / `xi.cap.provider`, doc 14) are formalized as documented ABI in the same break, and `host_api->read_image_file` is evicted into the `xi.image.decode` capability (net ABI bill: −1 slot; the pilot's `get_interface` route needs zero new slots, so no `register_capability` slot is required) | Any plugin DLL **you** own that only implements the Record `process()` stops loading at v12; any plugin of yours calling `read_image_file` stops compiling | Add the pack door NOW (bilingual — it coexists with your Record door today); swap `read_image_file` calls for the `xi.image.decode` capability (§3.7). Recompile against v12 at the cut | **M** per plugin for the door (measured on shipped plugins: blob_analysis +77 lines, mock_camera +29); **S** for the read_image_file swap |
| 2 | **`xi::Record` deleted from the script SDK** (and with it DocRegistry, COW, share/adopt internally — invisible to you except as the API deletion) | Every script idiom in §4.1's left column fails loud at compile: `t.image()`, `t.meta()`, `xi::Record()`, `use().process(Record)` | Port per the idioms table (§4.1). All target surfaces exist today | **S–M** per script (mechanical; ~15–60 min each in our ports of the examples tree) |
| 3 | **`xi::state()` deleted; cross-frame script state is `xi::kv()`** (doc 16) | Scripts using `xi::state()` fail loud at compile; an unported script's state resets once at its porting reload | Port with the self-seed pattern (§4.3) during the window so the carry is seamless | **S** per script |
| 4 | **XEX1-v3 becomes the default preview wire.** v1 is retained one release behind a flag, then deleted (window length = your call, §6.2) | Only a third-party XEX1-**v1** decoder you wrote yourself. In-tree decoders (expose webUI, `examples/lib/xex1.py`) are already bilingual v1+v3 | Port your decoder to v3 — it is plain tagged msgpack (`{v:3, channel, seq, frame:{key:[tag,value],…}}`), readable by any stock msgpack library; golden fixtures under `protocol/fixtures/binary/` | **M** if you own a decoder (a day incl. tests); zero otherwise |
| 5 | **`hello.abi` 1 → 2** — the long-planned protocol stamp bump, in lockstep with the clients | Nothing today (no client of yours checks the field — which is the problem this fixes) | If you own a WS client: read `hello.data.abi`, compare, warn loudly on mismatch. One comparison | **S** |
| 6 | **`record_save`'s legacy `.json`+`.bmp` export door is deleted** (it goes with Record) | New runs no longer produce `.json`+`.bmp` exports. Existing files stay on disk, stay human-readable — and stay what they always were: exports, never replayable (doc 13 §2) | Switch capture workflows to `.xex1` now (§3.9). If you have OLD captures you genuinely need to REPLAY, raise it — §6.4 | Zero, unless §6.4 applies |
| 7 | **Five WS commands retire — pending YOUR confirmation** (doc 11): `watchdog_status`, `unload_script`, `unquarantine_plugin`, `load_plugin`, `get_project`. Zero in-tree callers; all five are plausible external-operator surface, so none retires without your sign-off | Any external script/tooling of yours calling one of the five | Grep your tooling for the five literals; confirm or veto per command (§6.3) | **S** (an afternoon of grepping) |

### What does NOT change (so you don't re-audit it)

The verdict plane (`xi::ok/ng/result`, run_result shape), the control plane
(create/def/prepare/commit/exchange, project open/save), `project.json`
(`xi.project/1`, unchanged), the health contract (`xi.health/1`), params and
instance UIs, the trigger model and gathering sources, the WS envelope shape,
auth. THE CUT is a data-currency swap, not a protocol redesign.

## §2 Why one train, not three

The pieces are coupled: the pure-door ABI needs plugins ported; deleting Record
needs scripts ported; the v3 wire default and the `abi` stamp are each
one-client-visible-moment events. Bundling them means you re-validate your app
ONCE. Everything that could be decoupled already was — it shipped bilingual and
is behind you.

## §3 The bilingual window — what you can adopt TODAY

Everything below runs against the current backend, coexists with your existing
Record code, and is exactly the code that survives the cut. Recommended order:

1. **Read the frame as a pack** — `auto f = t.pack()` beside your existing
   `t.image()`. Typed reads return `std::optional` (absence is explicit):
   `f.get_i64("seq")`, `f.get_image("frame")` (zero-copy span + dims — wrap it
   in a `cv::Mat` directly). *(S per script)*
2. **Build packs script-side** — `xi::ScriptPackBuilder` (`add_i64/f64/str/
   bool/bin`, `add_image`, `add_mp(xi::mp::Writer)` for one nested subtree,
   then `seal()`). *(S)*
3. **Chain into plugins** — `xi::use("det0").process(pack)` drives a plugin's
   pack door, request-reply, same host funnel and crash gates as always. *(S)*
4. **Expose from script** — `xi::use("expose").push(pack)` (with a
   `$channel` entry for UI routing): fire-and-forget, staged and flushed in
   frame order for sink targets (doc 17). *(S)*
5. **Ordering identity** — where you relied on the host stamping `$seq` on
   ordered sinks, stamp it yourself before seal: `b.add_i64("$seq",
   xi::run_id())` — the same value the host used to inject (doc 17). *(S)*
6. **Fault discipline** — check `r.is_fault()` / `fault_reason()` before
   reading results; one poison marker, `$src`/`$prov` provenance rides along,
   and the host short-circuits fault inputs so your plugin never sees poison
   (doc 15). *(S)*
7. **Capability calls from your plugins** — `get_interface("xi.cap", 1)` →
   `call("xi.jpeg.encode", …)` / `"xi.image.decode"`; dedup'd, host-forwarded,
   crash-attributed (doc 14). Probe with `$probe:true`; version in-band with
   `$v`. *(S per call site)*
8. **`xi::kv()` state** — port with the self-seed pattern (§4.3). *(S)*
9. **Record `.xex1` captures now** — `record_save`'s pack door writes
   XEX1-v3; `record_replay` re-emits it byte-lossless. Every capture you make
   during the window is already in the surviving format. *(S — config)*
10. **Take your plugins bilingual** — add the `xi.pack@1` door beside your
    Record `process()`; ship both until the cut. *(M per plugin)*

Live QA references you can crib from: `examples/qa_use_pack_door` (build →
chain → push in one script, zero Record), `qa_pack_record_replay` (record →
save → replay, byte-lossless), `qa_pack_order` ($seq + ordering),
`qa_pack_fault_path` (fault propagation), `qa_kv_reload` (kv + hot-reload
migration), `qa_cap_imgcodec` (capability plane).

## §4 The port checklist

### 4.1 Scripts — Record → Pack idioms

| Today (Record) | After (Pack) |
|---|---|
| `auto img = t.image("frame");` | `auto f = t.pack(); auto img = f.get_image("frame");` — zero-copy pixels + dims; `cv::Mat` wraps the span directly |
| `t.meta()["seq"].as_int(0)` | `f.get_i64("seq").value_or(0)` — metadata rides as ordinary pack entries |
| `det.process(xi::Record().image("gray", g).set("thresh", v))` | `xi::ScriptPackBuilder b; b.add_image("gray", g); b.add_i64("thresh", v); auto r = xi::use("det0").process(b.seal());` |
| `result["blob_count"].as_int(0)` | `r.get_i64("blob_count").value_or(0)` — typed, optional-returning |
| Nested trees: `rec["items[0].score"]` | One `add_mp(key, xi::mp::Writer)` subtree at build; read via `r.get_mp(key)` + `xi::mp::Reader`, or enumerate unknown packs with `r.for_each(...)` |
| `rec.is_na()` / `na_reason()` | `r.is_fault()` / `fault_reason()` — check BEFORE reading results; `src()`/`prov()` name the producer and the hop chain (doc 15) |
| `xi::use("expose").process(xi::Record().set("$channel","x").image(...))` | Build a pack with `b.add_str("$channel","x")` + entries; `xi::use("expose").push(pack)` |
| `use(sink).process(rec)` (host staged it) | `use(sink).push(pack)`. Note: `process()` on a declared ordered sink is now REJECTED fail-loud (empty pack + a log naming the fix) — push is the sink feed, process is the reply chain (doc 17) |
| Host-stamped `$seq` on ordered sinks | Producer stamps: `b.add_i64("$seq", xi::run_id())` before seal — same value, honest ownership |
| `xi::state()["count"]` | `xi::kv().get_i64("count")` — see §4.3 |

Declared-keyset reads (compile-checked keys) exist on the pack side too —
`ScriptTypedPack` with a schema of key slots (see `examples/qa_pack_walk`).

### 4.2 Replay files — the doc 13 rulings, compressed

| You have | Ruling | Your move |
|---|---|---|
| `.xex1` recorded during the window (v3) | The durable format; replays before and after the cut | Nothing — this is the target |
| Tagless "v2 draft" `.xex1` (dev-line only, pre-2026-07-03) | Refused permanently — a converter would have to guess types by shape, the exact defect v3 removes | Re-record. (These never left our dev line; you almost certainly have none) |
| Record-era `.json`+`.bmp` captures | Never replayable (no reader ever existed); remain human-readable exports forever | Nothing — unless you need to REPLAY specific old captures: then §6.4 |
| XEX1-v1 frames saved off the wire | A lossy *display* format, never a record of the data plane | Nothing; v1 decoders keep decoding them for as long as v1 decoders exist |

### 4.3 State — the JSON → kv self-seed (doc 16)

Both channels are live in one DLL during the window; the host converts
nothing. A porting script seeds once, then the kv channel carries:

```cpp
// first inspect after the porting reload:
if (xi::kv().empty() && xi::state().has("count"))
    xi::kv().set_i64("count", xi::state()["count"].as_int(0));
```

From then on, use `xi::kv()` everywhere (typed get/set, `XI_KV_SCHEMA(N)` +
`xi::set_kv_migrate` replacing the old code_change hook). At the cut the
Record channel disappears; the seed line dies with it (delete it after the
port soaks). State is in-memory only — worst case for a script you never
port is one counter reset at its porting reload, not data loss.

## §5 Rollback stance

- **The train is one event on our side**: one merge, one revert point.
  Pre-cut `master` stays tagged; artifacts are zip-swappable and
  self-identifying (`compat-manifest.json` names backend version, ABI, and
  the tested-together client set).
- **The wire has a flag**: XEX1-v1 remains selectable for one release after
  the flip — the display-wire rollback is a config change, not a redeploy.
- **The real safety net is the window, and it is honest to say so**: code you
  port during the window runs on BOTH pre-cut and post-cut backends, so
  rolling the backend back does not roll your porting work back. After the
  cut, the only expensive rollback is un-porting — which is precisely why the
  porting and the soak happen BEFORE the train, and the train itself is a
  recompile + revalidation, not a leap.
- **Nothing durable needs a rollback story**: `.xex1` files are valid on both
  sides of the cut; state is in-memory; `project.json` is untouched.

## §6 Open coordination decisions — what we need from you

1. **The train date.** Our gates are green; pick the window that matches your
   release rhythm. Every pre-train release is one more deployed client to
   re-touch, so sooner is cheaper — but there is no forcing function.
2. **XEX1-v1 support window.** Proposal: default flips at the cut, v1 behind
   a flag for ONE release, then deleted. Veto with a reason if you need
   longer.
3. **The five command retirements** (§1.7). Confirm or veto EACH of:
   `watchdog_status`, `unload_script`, `unquarantine_plugin`, `load_plugin`,
   `get_project`. "No answer" keeps a command alive — we retire nothing
   unconfirmed.
4. **The `.json`+`.bmp` → `.xex1` converter** (doc 13 §2). Built ONLY if you
   name concrete pre-cut captures you must replay; otherwise the answer is
   re-record and the converter is never built. Decide on the train, not
   after.
5. **A named contact + a rig window** for the joint validation pass — same
   ask as the previous train.

### §6 — ANSWERED (app team, 2026-07-04). All five closed:

1. **Train date**: app side is ported + soaked and ready NOW; the date is
   ours to pick by release rhythm — no constraint from them.
2. **XEX1-v1 window**: proposal ACCEPTED (flag for one release, then delete).
   Their decoder is already v1+v3 bilingual.
3. **Command retirements**: ALL FIVE CONFIRMED (`watchdog_status`,
   `unload_script`, `unquarantine_plugin`, `load_plugin`, `get_project` —
   zero usage, grep-verified on their side). Retirement rides the train.
4. **Converter**: NOT NEEDED — no legacy capture replay dependency (they use
   `.xex1` synth/test data). The converter is never built (doc 13 ruling
   stands as re-record).
5. **Contact**: ct@xception.tech (app-team lead); rig window with the train.

**Their window work, already done (FYI)**: 4 app-owned measurement plugins
(shape_based_matching / lens_calib / pose_register / dim_measure) are
bilingual with pack==Record parity tests green (their branch
`feat/pack-door-v12`); inspect scripts + their sources (cmd_panel /
folder_camera) fully pack-ported; wire running frame_wire v3.

**Enablement asks from them (our side, tracked)**:
- **E1 — capability plane not live in their deployment**: `xi.image.decode`
  factory-time resolve returns null (their deployed backend exe predates
  `xi.cap.provider@1`), so folder_camera falls back to `read_image_file`.
  Fix = redeploy backend + plugin DLLs from the gate-green tip, and create
  an `imgcodec` instance in the project (registration happens in the lib
  plugin's factory — see `examples/qa_cap_imgcodec/instances/codec/`).
  Machine-level autoload for lib plugins (no per-project instance) is a
  named v12 candidate (doc 14).
- **E2 — full-resolution preview without raw-pixel WS load**: they downsample
  to ~384px today. **DELIVERED** (`polaris2/e2-jpeg-preview`): expose resolves
  `xi.jpeg.encode` via `get_interface("xi.cap",1)` (per-instance, cached,
  re-resolve tolerant) and, on its v3 **WS-SEND** path only, each IMAGE entry
  carries a nested-mp `preview` map `{w,h,c,enc:"jpeg",q,data:<bin>}` beside the
  frozen v3 image tag (the tag is untouched; raw px leaves the wire). Gated by
  the `preview_compress` def knob (default ON) and the capability's presence —
  **fail-OPEN to raw**, never to nothing: absent provider / `preview_compress`
  off / a per-image contract `$fault` → that image ships raw; a codec-wide
  funnel failure (`on_fault:"refuse"` quarantine → rc<0) flips a **persistent
  degraded** raw mode (one status line + one log per transition, throttled
  re-probe — no per-frame fault storm). Dims come from the SOURCE image (the
  encoder reply carries none). `record_save`/disk dumps stay RAW via the SHARED
  `xex1_pack_dump.hpp::encode_pack_v3` (the substitution lives in expose's own
  `xex1_wire_preview.hpp`), so record→replay byte-identity is intact
  (`record_replay_pack_test` green). Proof: `examples/qa_jpeg_preview`
  (full-res dims via JPEG SOF, size≪raw, dedup `encodes==1` across two channels,
  raw fallback + degraded-mode legs). Client decoders updated: the expose webUI
  (`ui/index.html`) and `examples/lib/xex1.py`.
- **Deployment hygiene incident (noted)**: the main-checkout deployed
  `xi-expose.dll` was a stale record-only build (source had the door, DLL
  predated it); they hot-swapped our integration build to unblock validation
  (backup `.record-only.bak-20260703`). Official redeploy from the gate-green
  tip owed — and binaries-lag-source is exactly the class the sdk/live gate
  stages were built to catch in-repo; deployment needs the same discipline.

## §7 Reference

- Scope + gates + the cut's exact contents: `docs/new_gen/10-pack-migration-scope.md`
- Parity evidence (what "everything works pack-only" is measured against): `docs/new_gen/12-pack-parity-matrix.md`
- Replay-file rulings in full: `docs/new_gen/13-replay-file-migration.md`
- Capability plane: `docs/new_gen/14-lib-plugin-capability-plane.md`
- Fault/provenance semantics: `docs/new_gen/15-pack-fault-semantics.md`
- State (`xi::kv()`) decision + the cut's exact state edits: `docs/new_gen/16-script-state-shape.md`
- Ordering + `xi::run_id()`: `docs/new_gen/17-pack-ordering-semantics.md`
- Wire truth: `contract/` schemas + `protocol/fixtures/` (incl. the XEX1-v3 binary goldens)
