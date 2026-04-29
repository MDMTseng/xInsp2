# multi_source_surge2 — friction findings

FL r6 regression sub-round. The three fixes from PR #22 all hold (see
RESULTS.md). What follows is *new* friction surfaced by writing this
regression — i.e. things PR #22 didn't introduce but didn't address,
or small drift around the fixes themselves.

Severity scheme (same as prior rounds):
- **P0** — wrong / data-loss / crash; blocks usage
- **P1** — significant ergonomic / correctness gap; user is likely to
  trip
- **P2** — papercut / docs gap / minor surprise

---

## P2 — Mojibake in WS `log` warn payload (and cl.exe diagnostics) on CJK Windows

**Symptom.** The PR #22 watchdog warn line, when received by a Python
client and printed to a CP-950 console, shows the em-dash as garbled
chars:

```
[warn] dispatch_threads=4 ▒X script watchdog disabled under N>1 ...
```

The byte sequence on the wire is the proper UTF-8 `\xE2\x80\x94` —
the WS log payload IS valid UTF-8. The mojibake is purely on Python's
default stdout encoding side. Same class of issue affects the cl.exe
diagnostic message bodies surfaced through `ProtocolError.data
["diagnostics"]` when the backend ran without `VSLANG=1033`: the
payload is whatever cl.exe emitted (Big5 / CP-950 bytes
mis-interpreted as latin-1), and the SDK passes it through verbatim.

**Repro.**
```
chcp     # 950 on this box
python examples/multi_source_surge2/driver.py
# Read the FIX 2 line; em-dash is mojibake.
```
And (separately) `inspect_collision.cpp` produces `error C2374:
'foo': ���Ʃw�q; �h�Ӫ�l�]�w` — the diagnostic text body is
unreadable on a non-CJK consumer. The C2374 *code* is fine; the
description body is not.

**Root cause.** Two different bugs sharing one symptom class:

1. The em-dash one is a Python-side encoding choice
   (`sys.stdout.reconfigure(encoding='utf-8')` would fix it for the
   driver), but it bites every agent / human who runs
   `examples/.../driver.py` on a Chinese Windows. Worth a one-line
   recipe in `tools/xinsp2_py/README.md` or even
   `Client.__init__` opt-in.

2. The cl.exe one is the same class as the FL r2 fix
   (`c52e49e — cl.exe diagnostics no longer get stripped to '?' on
   non-en-US Windows`). That fix preserved the bytes; it didn't
   transcode them. If the backend runs without `VSLANG=1033` (e.g.
   when a user starts it from the command line on a CJK box), the
   compiler emits CP-950 and the SDK's enrichment leaves them as-is.

**Severity.** P2. The English-language portion of every diagnostic
(file path, line number, error code) is intact, which is enough for
an agent to diagnose. But for a human author trying to read the
message body on a non-CJK locale, it's noise.

**Who fixes.** Two-prong:
- SDK side: tag the enriched message header with a hint when it
  detects bytes outside printable ASCII in diagnostic bodies (1-line
  print suggesting `VSLANG=1033`).
- Backend side: consider invoking cl.exe with `VSLANG=1033` always
  (set via the spawn env), not relying on the parent's environment.
  Already documented as a workaround in
  `docs/guides/writing-a-script.md`'s "mojibake" subsection but the
  backend doesn't enforce it for its own subprocess.

I did NOT fix this in the regression — it's a backend behaviour
choice and the user's PR-#22 brief said "don't touch backend
dispatch / IPC; write up backend bugs."

---

## P2 — `xi::Param<T>` reload-on-recompile vs `set_param` racing

**Symptom.** Driver does `compile_and_load(inspect.cpp)` then
`set_param("slow_mode_ms", 50)`. If the driver opens a fresh project
between the two, the param is set on the post-reload Param, fine.
But if the project is the same and inspect was *already* loaded with
slow_mode_ms=0 from a previous sweep's set_param call, the new
`compile_and_load` re-prepares the param table — and on this box the
param value carried over, but I'm not 100% sure it would across all
backends. The replay path documented in PR #17 should cover it, but
no test currently asserts "Param value survives intra-session
recompile of the same project."

**Repro.** None reliable in this regression — sweeps either don't
flip slow_mode_ms or do `open_project` in between, both of which
mask the question.

**Severity.** P2 — gap in the regression coverage rather than a
known bug.

**Who fixes.** Could be a one-liner additional sweep in
`examples/multi_source_surge2/driver.py` that does a within-sweep
`set_param` followed by a `compile_and_load` of the same script and
asserts the value is still in effect. Out of scope for the r6
regression itself.

---

## P2 — Watchdog warn omits the cmd:start args

**Symptom.** The warn-log message says `dispatch_threads=4` but
doesn't mention what watchdog ms value tripped the warn. So on a
follow-up driver that only inspects the WS log inbox, you can't tell
whether the warn fired because of a `set_watchdog_ms 1` (effectively
already-tripping) or `set_watchdog_ms 600000` (the cap). One of those
indicates the user fat-fingered the watchdog ms; the other is a
deliberate choice. Today the warn says only "watchdog disabled under
N>1," which is the right policy but loses the input value.

**Repro.** See driver.py FIX 2 sub-test: warn payload is constant
modulo `dispatch_threads`.

**Severity.** P2 — papercut; users debugging the warn on a real
project would have to query `cmd:watchdog_status` separately to
correlate.

**Who fixes.** Backend. One-line edit to
`backend/src/service_main.cpp` warn formatter to include
`watchdog_ms=N` in the message; matches the existing `dispatch_threads=N`
pattern. Out of scope for me here (touching backend log payload format
brushes up against cmd:start path).

---

## P2 — Sweep harness conflates "no warn" with "warn dropped"

**Symptom.** In `test_watchdog_warn`'s negative control, I assert
`len(wd_warns2) == 0` after `set_watchdog_ms(0); cmd:start`. That's
the right negative test, but if the WS read loop was lagging the
log inbox by > 200ms (the post-stop sleep), the assertion would
spuriously pass. The SDK doesn't expose a "drain log inbox up to
this seq" primitive — `_inbox_logs` is a `Queue` keyed only by
arrival order.

**Repro.** Hard to repro without a slow loopback; on this box
the log arrives within 5ms of `cmd:start`'s rsp.

**Severity.** P2 — not a real bug today, but the negative test is
weaker than it looks.

**Who fixes.** Could be a small SDK helper:
`Client.flush_logs(timeout=0.1)` that reads-with-timeout until empty.
Or attach a sequence number to log messages on the wire so a client
can fence them against an `id`. Out of scope here.

---

## Friction NOT found (deliberately checked)

These were on my watch list because PR #22 touched them; the
regression turned up no new symptoms:

- **Per-source attribution under N=4** — zero "unknown" tags, zero
  "no_sources" errors across 821 active inspects in S4. FNV routing
  fine.
- **drop_newest counter discrimination** — S5 reports
  `dropped_newest=220, dropped_oldest=0`; S4 reports the inverse.
  No cross-contamination.
- **Hot reload mid-dispatch** — not exercised in surge2 (already
  covered by surge1); skipped to avoid duplicating coverage.
- **Watchdog disabled under N=1** — `set_watchdog_ms(500); start
  N=1` should NOT emit the warn. Implicitly checked because every
  N=1 sweep with no slow path ran with watchdog=0; if I add an
  N=1+watchdog>0 sweep later, that'd close the loop explicitly. P3
  test-coverage gap.
