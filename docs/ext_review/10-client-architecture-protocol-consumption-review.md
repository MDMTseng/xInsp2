# Client-Side Architecture and Protocol Consumption Review

| Field | Value |
|---|---|
| **Date** | 2026-07-02 |
| **Reviewer** | Claude (external advisory) |
| **Status** | Advisory |
| **Scope** | The five WS-protocol consumers — `ui-components/`, `hmi/`, `vscode-extension/`, `fe/include`, `tools/xinsp2_py` — their per-client code quality and, above all, the drift *between* them |

## Scope

This review looks at xInsp2 from the **client edge**: everything that speaks the
WebSocket protocol back to `xinsp-backend.exe`. Reviews 06 and 09 covered the
backend side of the transport and the ABI/version boundary; this is the mirror —
what the consumers actually do with those contracts.

Three questions drove it:

1. **Per-client quality** — architecture, reconnect/timeout handling, error
   handling, dead code, for each of the five surfaces.
2. **Cross-client drift** — how many independent implementations of the envelope
   and the binary frame exist, whether they agree field-by-field on a sample of
   messages (`hello`, `rsp`, `run_result`, `run_finished`, the `XEX1` preview
   frame), and where one consumer was updated for a protocol change and another
   was not.
3. **Test coverage quality** — whether the client tests exercise the *real*
   protocol (shared fixtures / a live backend) or hand-rolled mocks that can
   drift from it.

Every drift claim below cites `file:line` from **both** sides. Findings are
ranked by real risk: silent drift that corrupts operator-visible data first,
then capability gaps, then dead code and doc rot.

A calibration note up front, because it changes one of the five items: **`fe/include`
is not a protocol consumer.** The FE supervisor has no WS client or server — it
publishes its state to a JSON status file (`fe/include/xi/xi_fe_status.hpp:5-18`),
consumed by `backend/src/fe_main.cpp`, not over the wire. It is reviewed here only
for the client-facing *gap* it leaves (Finding 8), not as a wire consumer.

## Executive Summary

The transport story is **better than the sum of its parts on the happy path and
worse than it looks on the contract edges.** The generic envelope (`cmd`/`rsp`/
`event`/`log`/`instances`) is simple, and where two clients both consume a given
message they agree on its shape — there is no field-name or field-type
disagreement in the messages I sampled. The Python SDK in particular
(`tools/xinsp2_py`) is a genuinely good client: typed, fixture-tested, and
already caught up to the `run-outcome` wire changes. The `ui-components` shim is
clean and, unusually, its tests run against a *real* spawned backend rather than a
mock.

The problems are all at the **seams**:

- **The event contract is consumed wildly unevenly.** The Python SDK models the
  full `run_result` outcome contract (schema `xi.run-outcome/1`); the HMI consumes
  the raw `run_result`; the **VS Code extension — the primary developer client —
  consumes none of it.** It has no handler for `run_result`, `run_finished`,
  `run_error`, `state_dropped`, `compile_finished`, or `compile_started`. So the
  editor silently drops script state on a schema-mismatch hot-reload and never
  surfaces an NG/crashed verdict in continuous mode.

- **The protocol reference actively points consumers at files that do not exist.**
  `ws-protocol.md` tells a new consumer that the non-finite-restore and
  expose-decode logic live in `ui-components/src/protocol.mjs` and
  `tools/xinsp2_py/xinsp2/client.py`. Neither location contains that code; one of
  the two files does not exist at all. A consumer who trusts the spec builds a
  decoder that skips non-finite restore — the exact silent-corruption bug the spec
  spends a page warning about.

- **The "shared" transport is not shared, and abi is enforced nowhere.** There
  are four to five independent envelope parsers (the shim explicitly built to
  prevent that is bypassed by the HMI), each with its own reconnect timing, auth
  support, and 503 handling. No client checks `hello.data.abi` — the client half
  of review 06's finding.

- **The one genuinely hard wire format has zero cross-implementation test.** The
  `XEX1` binary frame is encoded and decoded by three independent hand-rolled
  msgpack codecs; the shared fixtures cover only the trivial text envelopes.

None of these is a data-integrity time bomb *today* (the codecs happen to agree,
the field shapes happen to match). They are drift the project has already started
to accumulate and has no mechanical guard against.

## Scorecard

| Dimension | Grade | One-line rationale |
|---|---|---|
| `ui-components` (shim + Svelte) | B+ | Clean generic shim, real-backend e2e + smoke tests; no auth; docs cite a `protocol.mjs` it doesn't ship. |
| `hmi` (operator dashboard) | B− | Works, good on-page diagnostics, reconnects; but hand-rolls its own socket instead of the shared shim, has a dead `safe_state` branch, reads compute-time `ms` as cycle rate. |
| `vscode-extension` | C+ | Transport is fine; consumes the *smallest* slice of the event contract — ignores the entire run-outcome + compile/reload lifecycle. |
| `fe/include` | n/a | Not a wire consumer (file-based status); reviewed only for the client-facing gap it leaves (Finding 8). |
| `tools/xinsp2_py` | A− | Best client: typed outcomes, fixture-tested, thorough error taxonomy; opaque binary by design; cannot authenticate. |
| **X:** message-shape agreement | B | Fields agree where consumed; the problem is uneven *coverage*, not disagreement. |
| **X:** version/abi negotiation | D | No client enforces `abi`; only one logs the version. |
| **X:** shared protocol layer | C− | Text fixtures are shared 3 ways; but 5 transports, 3 hand-rolled msgpack codecs, and docs pointing at phantom shared files. |
| **X:** binary (`XEX1`) consistency | C+ | Encoder + two decoders actually agree today; zero cross-impl test; a latent `fixmap`-only cap. |
| **X:** test coverage (real vs mock) | B | `ui-components` + `xinsp2_py` test the real protocol/fixtures; the binary path and the extension's event handling are untested. |
| **X:** auth / connection robustness | C | Extension does plain bearer only; Python and browser clients can't auth; no client distinguishes the 503 busy reject. |

## Findings

### 1. The protocol reference points consumers at decode/restore code that does not exist

`docs/reference/ws-protocol.md:118-127` tells a consumer that the shipped clients
restore non-finite sentinels via "`restoreNonFinite` / `restoreNonFiniteDeep` in
`ui-components/src/protocol.mjs` and `vscode-extension/src/protocol.ts`,
`_restore_nonfinite_deep` in `tools/xinsp2_py`", and `:357-359` says the stock
expose decoders live in "`ui-components/src/protocol.mjs` (`decodeExposeFrame`)
and `tools/xinsp2_py/xinsp2/client.py`".

Checked against the tree:

- `ui-components/src/protocol.mjs` **does not exist** (the src dir is
  `auto-panel.mjs`, `ws-client.mjs`, `components/`, `dashboard/`, `lib/`,
  `index.js` — no `protocol.mjs`).
- `vscode-extension/src/protocol.ts` exists but contains **no** `restoreNonFinite`
  / `restoreNonFiniteDeep` — it is the 75-line generic envelope only
  (`protocol.ts:1-75`).
- `tools/xinsp2_py/xinsp2/client.py` contains **no** `_restore_nonfinite_deep` and
  no expose/`decodeExposeFrame` decode — its binary path is explicitly opaque
  (`client.py:841-853`).

The code that actually exists is elsewhere and named differently: the JS decoder
is `decodeXEX1` + `restoreNonFinite` in `plugins/expose/ui/index.html:156,178`;
the Python decoder is `decode_xex1` + `_restore_nonfinite` in
`examples/lib/xex1.py:93,83`.

#### Consequence

This is the highest-risk item because the spec is *teaching the exact bug it
warns about*. A new client author reads the "consumers must restore the sentinel"
paragraph, goes to look at the cited reference implementation in the shipped SDK
or ui-lib, finds nothing there, and — reasonably — concludes the SDK already
handles it and calls `json.loads` / `JSON.parse` on the expose `json` string
directly. A non-finite measurement then reads back as the literal string `"NaN"`:
a JS threshold compare silently misfires, a Python compare raises `TypeError`
(`ws-protocol.md:118-127`, the project's own description of the failure). The doc
points them straight past the two files (`plugins/expose/ui/index.html`,
`examples/lib/xex1.py`) that would have shown them the pattern.

#### Recommendation

Correct the three file references in `ws-protocol.md` to the real locations, and
state plainly what is true: **the shipped SDK (`client.py`) and the shim
(`ws-client.mjs`) are transport-generic and do *not* decode or restore** — the
reference decoders are the expose plugin's own webUI and `examples/lib/xex1.py`. A
consumer that decodes expose frames itself owns the restore. This is a Bucket-A
truth-correction; it costs nothing and removes an active trap.

### 2. The VS Code extension consumes none of the run-outcome or lifecycle event contract

The extension's message handler (`vscode-extension/src/extension.ts:1101-1150`,
plus a second small handler at `:2765-2768`) dispatches exactly four server
message kinds: `event:hello` (logs the version and nothing else, `:1102-1103`),
`event:status` (`:1104`), `instances` (`:1118`), and `log` (`:1143`). A grep for
`run_result`, `run_finished`, `run_error`, `state_dropped`, `compile_finished`,
and `compile_started` across all 2893 lines of `extension.ts` returns **nothing**.
The only run feedback it shows is the `cmd:run` reply's `ms` field
(`extension.ts:1232`).

Compare the other two consumers of the same events:

- Python SDK: full typed `RunOutcome` over `run_result` with the schema, class,
  and system-code channel (`client.py:182-266`), and `RunFinished` over
  `run_finished` (`client.py:269-293`).
- HMI: consumes `run_result` (`hmi/app.mjs:307`) and `run_finished`
  (`hmi/app.mjs:300-304`).

#### Consequence

The extension is the *primary developer client*, and it is deaf to the parts of
the protocol that carry correctness signal:

- **`state_dropped` ignored.** The backend emits this after a `compile_and_load`
  whose new DLL declares a different state-schema version, precisely so a UI can
  tell the developer their persisted `xi::state()` was thrown away
  (`ws-protocol.md:225-241`). The extension shows nothing; the developer's state
  silently resets and they debug a phantom.
- **Verdicts never surface in continuous mode.** Every dispatch emits
  `run_result` with the ok/ng/na/crashed outcome (`ws-protocol.md:205-214`); the
  extension drops all of them. In `start` mode the editor cannot show that parts
  are failing.
- **`compile_finished` / `compile_started` ignored.** These exist so a client can
  show "compiling…" across the 3-5 s cold-compile quiet window without parsing log
  lines (`ws-protocol.md:216-223`). The extension relies on its own project-open
  `busy` flag (`extension.ts:1039-1068`) instead, which does not cover a
  developer-initiated recompile.

This is the sharpest example of the cross-client theme: a protocol addition
(schema `xi.run-outcome/1`) landed, the Python SDK and HMI were updated, and the
extension was not.

#### Recommendation

Wire `state_dropped` to a warning toast at minimum — that one is a correctness
signal a developer must not miss. Consuming `run_result` for a verdict indicator
and `compile_started/finished` for the progress affordance are quality wins but
lower urgency. This is a targeted event-handler addition, not the wholesale
`extension.ts` refactor already parked in Bucket D — it should not be conflated
with that.

### 3. No client enforces `abi`; each hand-rolls its own version posture

The `hello` event and `cmd:version` both carry an `abi` field for exactly this
purpose (`ws-protocol.md:22-25, 1026`). No client uses it:

- Extension: `event:hello` handler logs `backend v${msg.data?.version}` and
  returns — `abi` is never read (`extension.ts:1102-1103`).
- Python: `connect()` reads the `hello` data and returns it to the caller, but
  performs no check on `version` or `abi` (`client.py:384-387`).
- `ui-components` shim: `checkVersion` is opt-in, and even when set it tests only
  the `version` *string*, never `abi` (`ws-client.mjs:51-58`); the one caller that
  opts in passes a loose `/\d+\.\d+\.\d+/` regex that matches any semver
  (`ui-components/test/ws-client.smoke.mjs:26`).
- HMI: no version handling at all (`hmi/app.mjs:294-316`).

#### Consequence

This is the client half of review 06's "`abi:1` never enforced by any client".
The additive-only evolution rule (`ws-protocol.md:22-25`) means this is safe
*until the first genuinely breaking bump*, at which point every shipped client
will connect to an incompatible backend and misread frames with no diagnostic.
The mechanism to prevent that already exists on the wire and in one client
(`checkVersion`); it is simply unused.

#### Recommendation

Consistent with review 06's triage, this is a "when we cut the first breaking
bump" item, not a now item. The cheap down-payment: have each client log the
`abi` (not just `version`) at connect, and give the shared shim's `checkVersion` a
default that compares `abi` so the guard is one line away when needed.

### 4. Four-to-five independent transports; the shared shim is bypassed by the HMI

`ws-client.mjs` opens with its own charter: "the shared, GENERIC WS-client shim …
so nobody reimplements the transport" (`ws-client.mjs:1-11`). Yet the transport is
reimplemented in every other client:

- Extension: `vscode-extension/src/wsClient.ts` (its own `WebSocket` wrapper).
- Python: `client.py:795-853` (its own read loop).
- **HMI: hand-rolls a raw `WebSocket` in `hmi/app.mjs:269-317`** — it imports
  `xi-components.esm.js` for the *cards and layout* (`hmi/app.mjs:11-14`) but not
  the shim, so the one browser client that could trivially use `XiClient` does
  not.

The divergences that follow are exactly what a shared layer would have prevented:
reconnect backoff is 2000 ms in the extension (`wsClient.ts:22`), 1500 ms in the
HMI (`hmi/app.mjs:292`), and absent in both the Python client and `XiClient`
(neither auto-reconnects); binary handling, auth, and 503 posture all differ
(Findings 5-6).

#### Consequence

Bugs and protocol updates must be fixed 4-5 times. The envelope is simple enough
that the shapes have not drifted *yet*, but the reconnect/auth/error behaviour
already has, and that behavioural drift is operator-visible.

#### Recommendation

At minimum, point the HMI at the existing shim — it is the lowest-cost
consolidation and removes a whole hand-rolled parser (`hmi/app.mjs:294-316`) that
duplicates `XiClient._onMessage`. A fuller consolidation (extension + Python onto
generated envelope types) is a larger, post-1.0-shaped effort; the HMI one is not.

### 5. Auth is supported by one client, in one mode; the Python "remote" client cannot authenticate

The backend supports two handshake auth modes: plain bearer and HMAC challenge
(`ws-protocol.md:1163-1178`). Client support:

- Extension: sends `Authorization: Bearer <secret>` (`wsClient.ts:33-36`) — plain
  bearer only, no HMAC.
- Python SDK: `websocket.create_connection(self.url, timeout=...)` with **no
  header argument** (`client.py:368`) — it cannot send a bearer token at all.
- `ui-components` shim: `new this._WS(this.url)` with no headers
  (`ws-client.mjs:37`); the HMI likewise (`hmi/app.mjs:273`). (Browsers cannot set
  WS headers, so this is partly a platform constraint — but there is also no
  query-param or subprotocol fallback.)

#### Consequence

The README sells remote/LAN deployment with `--auth` and shows the developer
laptop connecting over the network (`README.md:298-311`), and the Python SDK is
documented as "a WS protocol client library" for exactly the remote/CLI case. But
a Python client **cannot connect to any backend started with `--auth`** — the one
scenario auth exists for. HMAC mode is reachable by no client at all.

#### Recommendation

Add a `header=[f"Authorization: Bearer {secret}"]` path to `client.py`'s
`connect()` (websocket-client supports it directly) and thread a secret through
the constructor — this is a small, self-contained fix that closes a real
capability hole. HMAC support across clients is a larger, demand-driven item.

### 6. No client distinguishes the single-client 503 "busy" rejection

The backend rejects a second connection with `HTTP 503` +
`X-Xi-Reason: single-client-busy`, and the spec explicitly says callers should
treat it as "another client owns the backend; retry after they disconnect" rather
than a health problem (`ws-protocol.md:1034-1047`). No client reads it:

- Extension: `wsClient.ts` never registers a `ws` `'unexpected-response'`
  listener; a 503 upgrade failure falls through to `'error'`→`'close'` and it
  reconnects every 2 s forever with no operator-visible reason
  (`wsClient.ts:58-68`).
- Python: the 503 surfaces as an `OSError` and `connect()` rewrites it into "can't
  reach xInsp2 backend … start it yourself" (`client.py:369-380`) — actively
  *misleading*, since the backend is up and healthy, just owned by another client.
- `XiClient`: `onerror` rejects with a bare "socket error" (`ws-client.mjs:41-44`).

#### Consequence

The single most common multi-client failure — an operator HMI and the extension
both attaching, or two HMIs — produces either a silent 2 s reconnect loop or a
"start the backend" message that sends the user to fix a backend that is running
fine. This is a diagnosability regression against a case the backend went out of
its way to make fast and labelled (`ws-protocol.md:1044-1047`).

#### Recommendation

Have each transport special-case the 503 / `X-Xi-Reason` and surface "another
client owns the backend" distinctly from "backend down". For the `ws` library
that is an `'unexpected-response'` handler reading `res.statusCode` and
`res.headers['x-xi-reason']`; for Python, inspect the handshake exception before
rewriting it.

### 7. The `XEX1` binary frame — the one hard format — has three hand-rolled codecs and zero cross-implementation test

The expose preview frame is encoded and decoded by three independent, hand-rolled
partial-msgpack implementations:

- Encoder: `plugins/expose/src/expose.cpp:46-79` (`mp_uint`/`mp_str`/`mp_bin`/…).
- JS decoder: `plugins/expose/ui/index.html:108-153` (`MsgpackReader`).
- Python decoder: `examples/lib/xex1.py:33-60` (`_mp`).

I checked them against each other by type: the encoder emits only `fixmap`,
`fixarray`/`array16`/`array32`, `fixstr`/`str8`/`str16`/`str32`, `bin8/16/32`, and
unsigned ints `fixint`/`0xCC`-`0xCF`; both decoders cover that full set. **They
agree today.** But two things make that agreement fragile and unverified:

- **No test exercises it.** The shared `protocol/fixtures/*.json` are all *text*
  envelopes, consumed by `backend/tests/test_protocol.cpp`,
  `vscode-extension/test/protocol.test.mjs`, and
  `tools/xinsp2_py/tests/test_run_outcome.py`. There is **no binary `XEX1`
  fixture** and **no encoder→decoder round-trip test** anywhere. The trivial part
  of the protocol has three-way fixture coverage; the genuinely hard part has none.
- **A latent cap.** `mp_map` emits `fixmap` only — `f.push_back(0x80 | (n & 0x0F))`
  (`expose.cpp:53`) — so a record with more than 15 top-level scalar keys silently
  wraps the count and corrupts the frame; neither decoder handles `map16`/`map32`
  either, so it would fail on both sides with no clear error. Not reachable by the
  current 5-key frame shape, but nothing guards it.

#### Consequence

The format most likely to drift (three hand-maintained codecs, evolving image/
value payloads) is the one with no mechanical check that the three stay in sync.
The next person who adds a field type to the encoder has nothing that fails if a
decoder can't read it.

#### Recommendation

Add one binary `XEX1` fixture to `protocol/fixtures/` (a captured real frame) and
a round-trip assertion in the JS and Python decoder tests — the same
shared-fixture pattern the text side already uses, extended to the hard case.
Optionally guard `mp_map` against `n > 15`.

### 8. `fe/include` is a file-based status surface with no client reading it

`fe/include` is not a wire consumer — the FE supervisor deliberately has no WS
client/server and instead rewrites a JSON status file on each transition
(`fe/include/xi/xi_fe_status.hpp:5-18`), consumed by `backend/src/fe_main.cpp`.
That design is sound and well-documented. The gap is on the *client* side of it:

- The HMI has a handler for an `event:safe_state` (`hmi/app.mjs:308`), but the
  backend emits no such WS event — `set_safe_state` was removed 2026-06
  (`backend/include/xi/xi_abi.h:382`). That branch is **dead code**.
- Nothing in any shipped client reads the FE status file. So the FE's careful
  distinction between a transient respawn and a latched `RespawnLimitExceeded`
  (`xi_fe_status.hpp:8-12`) — the whole reason the file exists — reaches no
  operator UI. The extension's attach mode knows a supervisor owns the backend
  (`extension.ts:2793-2799`) but reads none of its state.

#### Consequence

On a real line, when the FE gives up respawning a repeatedly-crashing backend, the
operator HMI shows a generic WS disconnect (and its `safe_state` handler, which
might have distinguished the case, is wired to an event that never fires). The
liveness signal the FE produces is stranded.

#### Recommendation

Delete the dead `safe_state` branch in `hmi/app.mjs:308` (truth-in-code, Bucket A).
Separately — and lower priority, demand-driven — decide the client contract for FE
state: either the FE status file is surfaced by a small reader in the HMI/extension,
or the backend re-exposes a distilled FE-liveness field over the WS status channel.

### 9. Minor: stale references and labels

- **`protocol.ts` cites a canonical schema file that doesn't exist.** Its header
  says "The canonical schema lives in `protocol/messages.md`. Shared fixtures … are
  parsed by both sides" (`protocol.ts:1-9`). `protocol/messages.md` does not exist
  (only `protocol/fixtures/`), and "both sides" is really three (C++, TS, Python)
  — `ui-components` parses no fixtures. Correct the pointer to `ws-protocol.md` +
  `xi_protocol.hpp`.
- **Orphan fixture.** `protocol/fixtures/vars_mixed.json` models the *removed*
  `vars` message and is asserted by no test (grep across the three fixture test
  suites returns nothing). It is dead weight that implies a live message; remove it.
- **HMI reads compute-time `ms` as cycle rate.** `hmi/app.mjs:302` stores
  `run_finished.data.ms` as `run_ms`; per `ws-protocol.md:196-202` this is inspect
  *compute* time only, not cycle latency, and consumers are told to migrate to
  `inspect_compute_us`. Low stakes on a dashboard, but it is the same mislabel
  review 05 flagged, now on the client side.
- **README version table lags.** `README.md:541` lists the Python client at
  `0.1.0`; `tools/xinsp2_py/pyproject.toml:3` is `0.2.0` and its CHANGELOG
  documents the `0.2.0` run-outcome catch-up. Bump the known-compatible row.

## What is genuinely good

- **The Python SDK (`tools/xinsp2_py`) is the model client.** Typed outcome
  parsing that is forward-tolerant by construction (`client.py:212-236`), a real
  error taxonomy (`CmdTimeoutError`/`ConnectionLostError`/`PartialStatusError`/
  `UnknownCommandError`, `client.py:84-149`) that turns wire ambiguities into
  catchable Python exceptions, honest docstrings that correct their own past
  drift (`client.py:481-485`), and a rename-tolerant metrics accessor
  (`client.py:571-582`). It is already caught up to the breaking `run-outcome`
  changes. This is what "consume the protocol well" looks like.
- **`ui-components` tests the real protocol.** The e2e specs spawn an actual
  `xinsp-backend.exe` and drive the page against a live socket
  (`ui-components/e2e/_serve.mjs:11,58`, `e2e/dashboard.spec.mjs:8-20`), and the
  shim smoke test drives the real orchestrator verbs end-to-end
  (`test/ws-client.smoke.mjs:19-52`). No hand-rolled backend mock to drift from —
  the strongest anti-drift posture in the repo.
- **The generic/plugin split is the right architecture.** Keeping the transports
  content-agnostic and pushing binary decode into the owning plugin's webUI
  (`ws-client.mjs:6-10`, `client.py:841-853`, `protocol.ts:7-9`) is a clean line.
  The problems above are drift *within* that model, not a flaw in it.
- **Shared text fixtures exist and are used three ways.** `protocol/fixtures/*.json`
  parsed by the C++, TS, and Python suites is exactly the right mechanism; the
  finding is only that it stops at the text envelope and doesn't reach the binary
  frame.
- **The HMI's on-page diagnostics panel** (`hmi/app.mjs:28-52`) is a thoughtful
  operator affordance — copyable logs without DevTools, opt-in via `?debug=1`.

## Prioritized Roadmap

**Phase 1 — truth corrections (Bucket A, cheap, land on master now)**
1. Fix the three phantom file references in `ws-protocol.md` (Finding 1) and the
   `protocol/messages.md` / "both sides" claims in `protocol.ts` (Finding 9).
2. Delete the dead `safe_state` HMI branch and the orphan `vars_mixed.json`
   fixture (Findings 8, 9).
3. Bump the README Python-client version row (Finding 9).

**Phase 2 — small correctness/capability fixes**
4. Wire `state_dropped` to a warning in the extension (Finding 2) — the one item
   with real correctness stakes for a developer.
5. Add a bearer-auth path to the Python `connect()` (Finding 5).
6. Special-case the 503 busy reject in each transport (Finding 6).

**Phase 3 — anti-drift infrastructure**
7. Add a binary `XEX1` fixture + round-trip decoder tests (Finding 7).
8. Point the HMI at the shared `XiClient` shim (Finding 4).
9. Extend the extension to consume `run_result` / compile lifecycle events
   (Finding 2, quality half).

**Deferred (demand-driven, post-1.0-shaped)**
- `abi` enforcement across clients (Finding 3) — the down-payment (log `abi`) is
  Phase 1-cheap; the enforcement is for the first breaking bump.
- HMAC auth across clients (Finding 5).
- FE-state client contract (Finding 8).

## Decision Checklist

- [ ] Do we accept that the VS Code extension surfaces no verdict and silently
      drops state on schema-mismatch reload, or is `state_dropped` a must-fix now?
- [ ] Is the Python SDK expected to work against `--auth` backends? If yes,
      Finding 5 is a bug, not an enhancement.
- [ ] Do we want one shared transport, or is per-client duplication an accepted
      cost pre-1.0? (Decides Finding 4's priority.)
- [ ] Should the binary `XEX1` format get the same shared-fixture treatment as the
      text envelope before more field types are added? (Finding 7.)
- [ ] Who owns surfacing FE liveness to an operator — the HMI reading the status
      file, or the backend re-exposing it over the status channel? (Finding 8.)

## Final Judgment

The clients are individually competent and collectively uncoordinated. Where a
message is consumed, it is consumed correctly; the risk is entirely in *coverage*
and *duplication* — the same protocol change landing in three clients and missing
the fourth, the same decode logic hand-rolled three times with no shared test, the
same reconnect/auth/error behaviour diverging because the shim built to unify it is
bypassed. The Python SDK shows the project already knows how to consume this
protocol well; the work is to hold the other four to that bar and to give the wire
contract a mechanical guard (shared fixtures for the binary frame, an `abi` check
one line from active) so the drift that has started to accumulate stops
accumulating silently. None of it is a rewrite; most of it is Bucket-A
truth-correction and a handful of targeted handlers.
