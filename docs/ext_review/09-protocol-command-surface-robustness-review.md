# Protocol and Command-Surface Robustness Review

| Field | Value |
|---|---|
| **Date** | 2026-07-02 |
| **Reviewer** | Claude (external advisory) |
| **Status** | Advisory |

## Scope

The WebSocket transport (`backend/include/xi/xi_ws_server.hpp`), the command
parser (`backend/include/xi/xi_protocol.hpp`), and the backend command surface —
the 53 handlers registered in `g_cmd_table`
(`backend/src/service_main.cpp:4505`) and dispatched by `handle_command`
(`service_main.cpp:4562`). I read the transport in full and a broad sample of
handlers chosen for risk: everything that touches the filesystem
(`compile_and_load`, `save_project`, `load_project`, `open_project`,
`get_dashboard`, `run`, `crash_reports`, the working-copy pair), everything that
mutates project/instance state (`set_instance_def`, `create_instance`,
`commit_group`, `prepare_instance`, `set_param`), and everything that parses
numeric or structured input. I also checked what the shipped Python client
(`tools/xinsp2_py/xinsp2/client.py`) assumes of the wire.

The trust model is calibrated per the README and the triage record: pre-1.0,
first-party only, default bind is loopback, remote mode is opt-in behind a shared
secret on a trusted LAN. Authn is graded against a lab network, not the public
internet. I did **not** re-open the "typed args for all 54 commands" question —
triage settled that (00-triage Bucket D). Findings here are specific handlers or
transport behaviours whose *robustness* (crash, corruption, silent drop, or an
undocumented assumption) is wrong independent of typing.

## Executive Summary

The transport is the strong part of this surface. `xi_ws_server.hpp` is a
careful, defensively-written single-client server: it caps inbound frames and
reassembled messages at 16 MiB *before* allocating, rejects unmasked and
oversized frames, enforces the RFC 6455 control-frame constraints, times out the
handshake against slow-loris, bounds the send direction with `SO_SNDTIMEO`, and
rejects a second client with a clean HTTP 503 instead of a SYN-timeout stall. The
scars of prior audits (P0-D5 slow-loris, FL r7 backlog, the send-timeout
backpressure path) are visible and healed. Most handlers are equally disciplined:
required string fields are null-checked, every call into plugin/script code is
wrapped in `catch (seh_exception)` + `catch (std::exception)` so a plugin fault
becomes a structured `rsp` error rather than a dead backend, and the numeric
inputs that feed real machinery (`fps`, `watchdog ms`) are clamped.

The gaps are narrower but real. **The single highest-value issue is that there is
no exception guard around command dispatch at all** — the serving loop calls
`srv.poll(100)` → `handle_command` → the handler with no `try`/`catch` anywhere
in the chain, yet `docs/reference/ws-protocol.md:986` promises "Exception inside a
command handler: `rsp` with `ok:false`". Every handler is individually careful
about *plugin* faults, but a plain `std::exception` escaping any handler (most
plausibly `std::bad_alloc` from the unbounded whole-file reads in `get_dashboard`
/ `crash_reports`) terminates the whole process. Below that: a malformed command
envelope produces only a `log`, never a correlated `rsp`, so the client blocks to
its timeout; and the filesystem commands accept arbitrary absolute paths with no
containment, an asymmetry with the containment guard `compile_and_load` *does*
apply, and an undocumented assumption in remote mode.

None of these is a public-internet emergency at this stage. The exception-guard
gap is the one I would fix before the next tagged build because its blast radius
is the entire backend and it contradicts a written contract.

## Scorecard

| Dimension | Grade | One-line rationale |
|---|---|---|
| Transport framing & limits | A− | 16 MiB caps enforced pre-alloc, masking/control-frame checks, fragmentation bounded; outbound uncapped but plugin-trusted. |
| Connection / session lifecycle | A− | Single-client 503, slow-loris handshake timeout, send-timeout backpressure, reconnect clears error ring — genuinely hardened. |
| Per-handler input validation | B | Required fields null-checked, plugin entry SEH-guarded, key numerics clamped; a few substring-scan flag parses are fragile. |
| Error reporting consistency | C+ | Structured `rsp` errors are the norm, but a malformed envelope drops silently (log only) and the documented handler-exception→`ok:false` contract is not implemented. |
| Path handling | C+ | `get_dashboard`/prebuilt-DLL are contained; `save`/`load`/`open_project` + `run` frame_path take arbitrary absolute paths, undocumented. |
| Resource exhaustion | B− | No unbounded per-client queue (synchronous send); handler buffers fixed; but two handlers read whole files into memory unbounded, and with no top-level catch that is process death. |
| Authn / authz posture | B | Bearer + HMAC-challenge, constant-time compare, loopback default, no-auth-on-0.0.0.0 warning; no authz tiers and plaintext post-handshake, both acceptable & documented for a lab. |
| Doc vs implementation drift | B | Command count and removed shapes well-documented; the exception-handling contract and one flag-parse variance drift from code. |

## Findings

Ranked by real risk under the lab trust model. "Crash the backend" outranks
"theoretical hardening", as instructed.

### 1. No exception guard around command dispatch; contradicts the documented contract (crash-the-backend)

`docs/reference/ws-protocol.md:986` states: *"Exception inside a command handler:
`rsp` with `ok:false, error: <what()>`."* No such guard exists. The chain is:

- the serving loop `while (...) { srv.poll(100); ... }` (`service_main.cpp:5185`) —
  no `try`/`catch`;
- `on_text = [&](std::string_view s) { handle_command(srv, s); }`
  (`service_main.cpp:4830`) — none;
- `handle_command` (`service_main.cpp:4562`) — looks up the handler and calls
  `it->second(srv, id, &*parsed)` (`:4577`) with none;
- `read_pending` → `on_text` inside `poll` (`xi_ws_server.hpp:662`) — none.

Handlers are individually rigorous about **plugin** faults (every `set_def` /
`exchange` / `commit` / `process` call site has `catch (seh_exception)` +
`catch (std::exception)`). But any `std::exception` thrown by *handler-owned* C++
— outside a plugin call — propagates out of `main`'s loop, hits
`std::terminate`, and (with the crash filter still armed) writes a minidump and
exits. The FE reads that as a backend crash.

The reachable trigger is Finding 4: `cmd_get_dashboard_` and `cmd_crash_reports_`
read entire files into a `std::string` with no size ceiling, and the string
concatenation that builds large replies (`image_pool_stats`, `list_plugins`,
inlined crash JSON) can throw `std::bad_alloc` under memory pressure. Any of these
takes down the process instead of returning `ok:false`.

#### Recommendation

Wrap the handler call in `handle_command` in one `try`/`catch (const
std::exception& e)` that emits `send_rsp_err(srv, id, e.what())` and a
`catch (...)` that emits a generic error — exactly what the doc already promises.
This is ~6 lines at `service_main.cpp:4576-4577` and it converts every latent
handler throw from "whole-backend death" into the documented structured error.
The per-handler plugin-fault guards stay as they are (they carry richer,
per-instance context); this is the backstop the contract assumes.

### 2. Malformed command envelope drops silently — no correlated `rsp`, client blocks to timeout

`parse_cmd` (`xi_protocol.hpp:310`) returns `std::nullopt` when `type != "cmd"`,
when `name` is absent (`:328`), or when `id` fails `std::stoll` (`:325`) — which
throws for a missing, non-numeric, or over-`INT64_MAX` id. On `nullopt`,
`handle_command` (`service_main.cpp:4564-4570`) emits only a `log` frame
("malformed cmd: …") and returns. **No `rsp` is ever sent.**

The shipped Python client waits on a per-id queue and raises `CmdTimeoutError`
only after the full timeout — 30 s default, 180 s for `compile_and_load` /
`open_project` (`client.py:440,476,584`), and its own comment notes "the cmd is
NOT cancelled on the backend." So a client that sends an otherwise-valid command
whose `id` overflowed a JS-side bigint, or whose envelope has a typo, hangs for
tens of seconds to minutes with no signal beyond a `log` line it may not be
watching. Because `id` couldn't be parsed, the backend genuinely can't correlate
a reply — but the missing-`name` and wrong-`type` cases *do* often carry a valid
`id` that could be echoed.

#### Recommendation

Parse `id` independently and first; if `id` is present and parseable but the rest
of the envelope is malformed, reply `{ok:false, error:"malformed command"}`
carrying that `id`. Only when `id` itself is unusable fall back to the log-only
path (there is nothing to correlate to). This closes the common "valid client,
one bad field, silent multi-second stall" case without changing the genuinely
uncorrelatable one.

### 3. Filesystem commands accept arbitrary absolute paths with no containment (undocumented assumption)

`save_project` (`service_main.cpp:2727`), `load_project` (`:2794`), and
`open_project` (`:2931`) take a client-supplied `path`/`folder` and read/write it
directly (`xi::project::write_text(*path, …)` at `:2746`;
`xi::project::read_text(*path)` at `:2797`; `open_project(*folder, …)` at
`:2972`) with no restriction to any base directory. `cmd_run_`'s `frame_path`
(`:3106`) is handed straight to the image reader (`:3162`) — arbitrary file read
by absolute path.

This is notable because the codebase *does* contain path containment where the
author judged it mattered: `compile_and_load` refuses a prebuilt `.dll` outside
the open project folder (`:2307-2335`, with an explicit "loads arbitrary DllMain
in-process" rationale), the cl.exe command line rejects shell metacharacters, and
`get_dashboard` guards its `name` against `..` / separators (`:4186`). The
file-I/O commands have no equivalent. Under the documented remote mode
(`--host=0.0.0.0 --auth=<secret>`), any client holding the bearer secret can
write a `project.json` anywhere the backend process can write, and read any image
file on the factory PC via `run`.

The trust model *mostly* covers this — a client that can `compile_and_load` runs
arbitrary C++ in-process anyway, so full FS access is not a new capability. But
the calibration is explicit that *silent* assumptions are the problem: this one is
undocumented, and it is asymmetric with the containment the same file already
applies elsewhere.

#### Recommendation

Do not add heavy sandboxing pre-1.0 — instead, **document** in `ws-protocol.md`
that `save_project` / `load_project` / `open_project` / `run.frame_path` accept
unrestricted host paths and that remote mode therefore grants full FS access to
any authenticated client (i.e. the secret is a full-trust credential). If cheap
containment is wanted later, the `compile_and_load` `weakly_canonical` +
`lexically_relative` pattern (`:2314-2323`) is already the in-repo template.

### 4. `get_dashboard` and `crash_reports` read whole files into memory unbounded

`cmd_get_dashboard_` slurps the dashboard file with `ss << f.rdbuf()`
(`service_main.cpp:4192`) into a `std::string` with no size cap, then embeds it
verbatim in the reply. `cmd_crash_reports_` does the same per crash file
(`:3520-3522`) and concatenates *all* of them into one reply string. A large or
pathological file (a multi-GB `dashboard.json`, or a crash directory that filled)
drives a `std::bad_alloc` which — per Finding 1 — is uncaught and kills the
process. Even bounded, embedding an arbitrarily large file verbatim into a control
reply is a poor fit for a channel whose message cap is 16 MiB (the *send* side is
uncapped, so the frame goes out, but a hostile size still costs a full copy).

#### Recommendation

Cap both reads (e.g. refuse or truncate a dashboard/crash file over a few MiB and
report `found:false` / a `truncated:true` flag). This is independently worthwhile
and, together with Finding 1, removes the most concrete backend-death path on the
command surface.

### 5. Substring-scan flag parsing is fragile and inconsistent

Several boolean/optimize flags are detected by scanning the raw args string rather
than parsing the field, and the variants are applied inconsistently:

- `remove_instance` checks only `args_json.find("\"delete_folder\":true")`
  (`service_main.cpp:4135`) — **no space variant**. A client that sends
  `{"name":"det0","delete_folder": true}` (a space after the colon, which is legal
  JSON and what a pretty-printer emits) silently keeps the on-disk folder. The
  operator asked to delete and the backend replied `ok` without deleting.
- `graph_capture` (`:3581`) and `open_project`'s `working_copy` (`:2947`) *do*
  check both spacings, so the convention is applied unevenly.
- `compile_and_load`'s `optimize` detection (`:2353`) and the `working_copy`
  scan run over the *entire* args string, so a `path` value that happened to
  contain the literal `"optimize":true` would false-trigger. Unlikely in practice,
  but it is the same class of bug the `set_param` comment (`:2909-2916`) documents
  having already been bitten by.

#### Recommendation

Route these through the existing structured helpers — `find_key` +
value-token compare (as `set_param` was fixed to do), or yyjson for the handlers
that already open a doc — rather than `std::string::find` over the raw envelope.
At minimum add the space variant to `delete_folder` so its behaviour matches the
others; the current form is a quiet correctness bug, not just a style nit.

### 6. Slow-consumer send couples a laggy viewer to the inspection workers (bounded)

Sends are synchronous and serialized: `send_frame` holds `tx_mu_` and does a
blocking `::send` (`xi_ws_server.hpp:717-757`), bounded by `SO_SNDTIMEO = 1500 ms`
(`:341-344`). This is a deliberate, healed design — the comment explains it
replaced an unbounded block that could freeze every worker on a dead client. But
the residual behaviour is that a slow-but-alive UI (a viewer decoding MB-sized
`expose` JPEG frames) can stall the dispatch worker that is pushing to it, and via
`tx_mu_` any other worker trying to send, for up to 1.5 s before the client is
dropped. Under the default `dispatch_threads = 1` that is a 1.5 s line hitch per
slow send until the drop fires. There is no per-client queue, so this is a latency
coupling, not unbounded memory growth.

#### Recommendation

Acceptable at this stage — flag only. If viewer-induced hitches are ever observed
on a line, the fix is architectural (a bounded, droppable outbound queue for the
non-critical `expose`/preview broadcast path, off the `tx_mu_` used by control
replies), which aligns with 05 #19's "move viewer encode off the ordered result
path". Not worth building pre-demand.

### 7. Reply JSON built with ad-hoc escaping in two list handlers (trusted content, but a desync vector)

`cmd_list_instances_` concatenates backend instance names unescaped:
`"{\"name\":\"" + v.name + "\",\"plugin\":\"" + v.plugin_name + "\"}"`
(`service_main.cpp:3968`). `cmd_list_plugins_` uses a local `esc` lambda that
escapes only `"` and `\`, not control characters (`:4217-4219`), and embeds
`p.manifest_json` verbatim (`:4239`). If any of these strings carried a control
char or a quote — or if a plugin's `plugin.json` `manifest` block were itself
malformed JSON — the emitted frame is invalid JSON, and the client's parse of that
whole message fails.

In practice this is contained by upstream validation: backend instance names are
gated by `is_valid_instance_name` (alnum/`_`/`-`/`.` only, `xi_plugin_manager.hpp:2095`),
script-declared instance names come from compiled C++, and plugin manifests are
authored by the (trusted) plugin developer. So this is low risk today. It is worth
noting because it relies entirely on that upstream discipline holding, while the
rest of the backend consistently routes dynamic strings through
`xp::json_escape_into` (the `Rsp::to_json` trust-boundary comment at
`xi_protocol.hpp:84` names exactly this "ad-hoc concat" pattern as the one to
watch).

#### Recommendation

Replace the `esc` lambda and the raw `+` concatenations with
`xp::json_escape_into`, matching every other reply builder in the file. For
`manifest_json`, either validate it once at scan time or pass it through a
re-serializer so a malformed plugin manifest can't corrupt an unrelated command's
reply.

### 8. Minor documentation drift

- `ws-protocol.md:364` says "The backend implements ~60 commands" and the
  backlog/README elsewhere say ~54; the registered table
  (`service_main.cpp:4505-4560`) holds **53**. Harmless but worth reconciling to
  one number so a reader auditing coverage isn't hunting phantom commands.
- The `version` reply hardcodes `"abi":1` (`service_main.cpp:2250`); the doc
  correctly explains this is the *WS protocol* version distinct from the C
  plugin-ABI (v11). No drift in behaviour, just a field two different "ABI"
  numbers pass through — the doc handles it, keep it that way.

#### Recommendation

Fold into the same doc pass as Finding 1's contract fix.

## Prioritized Roadmap

### Phase 1 — before the next tagged build (cheap, high blast-radius)

- **Finding 1**: add the top-level `try`/`catch` in `handle_command`. ~6 lines;
  makes the documented contract true and removes whole-backend death from every
  latent handler throw.
- **Finding 4**: cap the whole-file reads in `get_dashboard` / `crash_reports`.
  Removes the most concrete trigger for Finding 1.
- **Finding 5**: add the `delete_folder` space variant (quiet correctness bug);
  ideally move the flag parses onto `find_key`.

### Phase 2 — documentation truth (no code risk)

- **Finding 3**: document the arbitrary-path FS reach of the file commands and
  that the remote secret is a full-trust credential.
- **Finding 2**: reply with a correlated error when `id` is present but the
  envelope is otherwise malformed.
- **Finding 8**: reconcile the command count.

### Phase 3 — on demand only

- **Finding 6** (slow-consumer decoupling) and **Finding 7** (escape the two list
  handlers). Neither has a present consumer; do them when a real line hitch or a
  richer-charset plugin name appears.

## Decision Checklist

- [ ] Is the documented "handler exception → `ok:false`" contract actually
      enforced by a dispatch-level guard? (Finding 1 — currently no.)
- [ ] Can any single command reply pull an unbounded file into memory?
      (Finding 4 — currently yes, two handlers.)
- [ ] Does a valid client sending one malformed field get a correlated error, or
      does it block to timeout? (Finding 2 — currently blocks.)
- [ ] Is the FS reach of `save`/`load`/`open_project`/`run.frame_path` documented
      as a full-trust remote capability? (Finding 3 — currently no.)
- [ ] Do all boolean-flag parses handle both `":"` spacings? (Finding 5 —
      `delete_folder` currently does not.)

## Final Judgment

This surface is in better shape than a 53-command hand-rolled protocol has any
right to be. The transport is genuinely hardened — the framing caps, masking
checks, handshake timeout, single-client 503, and send-timeout backpressure are
exactly the things that go wrong first in a naive WS server, and they are all
handled with visible audit history. The handlers show consistent discipline where
it counts most: every crossing into untrusted plugin/script code is
double-guarded, and the inputs that reach real machinery are clamped.

The one finding I would not ship past is Finding 1: the backend promises in
writing that a handler exception becomes a structured error, and it does not — the
guard simply isn't there, so a plain `std::bad_alloc` (reachable via the unbounded
file reads of Finding 4) drops the whole process. It is a six-line fix with a
whole-backend blast radius, which is the best possible cost/benefit on this list.
Everything else is either a bounded latency coupling, a trusted-content escaping
nicety, or a documentation-truth gap where the *behaviour* is defensible and only
the *silence* about it is not. For a pre-1.0 first-party lab tool, that is a
short, honest list. Fix the dispatch guard and the two unbounded reads, document
the path reach, and this facet is in good standing for a 1.0 hardening pass.
