# FRICTION_FUZZ — FL r7 fuzz survey

Friction encountered while building the four fuzz harnesses. Each
entry: assumption I made, friction I hit, suggested follow-up.

## P0 (the headline finding) — FIXED 2026-04-29 (PR `fix/r7-p0-reader-disconnect`)

> **Status: FIXED.** Root cause was subtler than the original write-up
> (the reader's `recv_frame` IS wrapped — the actual escape route was
> the constructor: when CREATE failed after `start_reader_()` had
> spawned the thread, the constructor threw without joining, and the
> implicit `~thread()` on member destruction called `std::terminate`).
> Fix wraps post-`start_reader_()` ctor body in try/catch + funnels
> failures through `shutdown_()`; also belt-and-suspenders catch-all
> in `run_reader_()`. Harness now reports 16/16 strategies survived,
> 0 fatal. See `docs/reference/ipc-shm.md` § "Reader thread failure
> modes" for the post-fix design.

### IPC reader thread does not catch the EOF exception

`backend/include/xi/xi_process_instance.hpp` runs an always-on reader
thread (started by `start_reader_()` around line 109) that calls
`ipc::recv_frame(pipe_)` in a loop. `recv_frame` throws
`runtime_error("pipe read EOF")` when the worker disconnects without
sending a frame. That exception is **not caught at the thread
boundary**, so when a worker dies abnormally — or simply closes the
pipe without speaking — the host process aborts via `std::terminate`.

Reproduction: build `evil_worker.exe`, swap it in for
`xinsp-worker.exe`, open any project with `isolation:"process"`. The
worker connects, then either sends bad frames or just closes — the
host crashes with `0xC0000005` (crash dump in
`%TEMP%\xinsp2\crashdumps\`).

15 of 16 fuzz strategies trigger this. The control case (zero frames
sent, immediate close) also triggers it: it's the EOF path that's
broken, not any specific payload.

**Suggested fix:** wrap the reader loop body in `run_reader_()` in
`try { … } catch (...) { mark adapter dead; break; }`. The host
already has the respawn machinery (see line ~560 in the same file)
for the case where a buggy plugin crashes its worker — the EOF case
should follow the same path.

## P1 — RESOLVED 2026-05-09

### WS accept loop briefly refuses connections under sustained malformed input

Original symptom: every ~50–200 iters of harness #1, opening a fresh
WS connection (for the liveness probe) returned `WSAECONNREFUSED` for
1–2 seconds, then succeeded.

**Root cause** (different from the original "accept loop is slow"
theory): the WS server is single-client by design. While one client
is connected, `select()` was not even watching `listen_`, so a 2nd
connection's SYN sat in the kernel queue until Windows SYN-retry
timeout (~21 s). With `listen(1)` the kernel rejected most SYNs fast
(the WSAECONNREFUSED the harness saw); with a longer backlog it would
have manifested as a true 21 s stall.

The harness was also at fault: it kept the main fuzz WS open while
opening a separate liveness probe, which the server's single-client
contract rules out.

**Backend fix** (`backend/include/xi/xi_ws_server.hpp`): always include
`listen_` in the select set; on `accept()` while `client_` is set,
send back `HTTP/1.1 503 Service Unavailable` with
`X-Xi-Reason: single-client-busy` and close. Documented in
`docs/protocol.md` § Single-client enforcement.

**Harness fix** (`harness_ws_cmd.py`): close the main fuzz WS before
the liveness probe, reopen after.

After both fixes: 1500-iter run completes with 0 findings; all 7
liveness probes pong cleanly.

## Friction items (not bugs, just rough edges that slowed the harness)

### F1 — `Client()` doesn't auto-spawn the backend

The task description said `Client()` "auto-spawns the backend". In
practice (see `tools/xinsp2_py/xinsp2/client.py:117`), `connect()`
just opens a WebSocket; if nothing's on `:7823`, it raises
`ConnectionRefusedError` with a hint to start the backend manually
(`backend/build/Release/xinsp-backend.exe &`). I added a
`BackendProc` context manager in `_common.py` to spawn-and-shutdown
the backend per-harness.

Suggested fix: a `Client(spawn_backend=True)` knob would have saved
~30 minutes of harness boilerplate.

### F2 — Backend always binds `127.0.0.1:7823`, no env override

For harness #3 we want to spin up a one-shot backend per fuzz iter
without conflicting with whatever else might be on the port. There's
no CLI flag or env var to change the WS port, so each harness has
to serially own the port. Two harnesses can't run in parallel — and
two iters within a harness can't overlap. We worked around it
sequentially.

### F3 — `xinsp-worker.exe` location is hard-coded relative to `get_exe_dir()`

Harness #3 needed to inject a fuzzer-binary in place of the legit
worker. With no env-var or CLI-flag override, we had to physically
overwrite `backend/build/Release/xinsp-worker.exe` (after backing up
the original). It works, but it's an ugly inversion of control —
and a parallel harness running on the same checkout would race.
Suggested env: `XINSP2_WORKER_EXE=<path>` honoured by
`service_main.cpp:3036`.

### F4 — open_project on a project that needs cl.exe is slow

Each `open_project` for `cross_proc_trigger` (used by harness #3)
takes ~3–10 s because it triggers a project-plugin compile. With 16
fuzz iters that's up to 2 min of wall clock just for setup. For
harness #2 we sidestepped this by writing fake plugin manifests with
`"dll": "fakeplug.dll"` and never compiling — the manifest-validation
layer doesn't reach the DLL load before reporting validation issues.
Worth documenting that pattern for future fuzz harnesses.

### F5 — `cmd:open_project` reports `Empty()` on backend crash

When the backend crashes mid-open (target #3), the SDK's
`call("open_project")` queue times out → `Queue.Empty()` propagates.
Caller has to compare `proc.poll()` to disambiguate "slow open" from
"backend crashed". A more informative SDK error here ("WS closed
mid-call: ConnectionResetError") would have made the failure mode
self-evident on the first run rather than the third.

### F6 — File with literal `\x00` byte in source is silently invalid

I included `"n\x00ÿ": 3` in a Python source string for harness_config.
Python writes the file without complaint, but later attempts to
import or run it fail with "source code cannot contain null bytes" —
without identifying where. Cost ~10 min to track down. Not a bug
in xInsp2; flagged here so future fuzz harness authors know to keep
adversarial null bytes inside `chr(0)` rather than literal `\x00`.

## Assumptions made (per the task's instruction to write down rather
than ask)

- Backend `:7823` is always the WS port; no env override exists. (Confirmed.)
- evil_worker.exe is built via the existing CMake build tree under
  `backend/build`. Added a new `evil_worker` target gated on `WIN32`.
- The "unknown" worker process protection (per `MEMORY.md`) does not
  apply because all child processes spawned by this harness are
  ones the harness itself started.
- "Don't fix bugs you find" → I did not modify `xi_process_instance.hpp`
  or `xi_ipc.hpp` despite being able to repro the crash.

## Out of scope (explicitly)

- Network-side fuzzing of the WS handshake (HTTP upgrade) — the
  task targets the post-handshake JSON cmd parser only.
- Fuzzing of the script-runner DLL load surface (PR-out-of-scope
  for r7).
- Concurrency fuzzing (multiple clients pounding at once). Single-
  client coverage is what we delivered; concurrent fuzz is a
  candidate for r8.

## Linux-port notes (per project policy)

`evil_worker.cpp` is currently Win32-only — same constraint as
`xi_ipc.hpp` whose `Pipe` class throws on non-`_WIN32`. I gated the
binary in `backend/CMakeLists.txt` with `if(WIN32)` and left a
`TODO(linux):` block at the top of `evil_worker.cpp` keyed off
`xi_ipc.hpp`'s POSIX port. When that header gains a POSIX backend,
the fuzzer can ride on the same change. See
`docs/design/linux-port.md` inventory entry under "fuzz harnesses".
