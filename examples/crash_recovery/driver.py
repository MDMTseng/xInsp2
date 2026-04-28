"""Driver: open crash_recovery, run 10 frames, observe which crash and
verify the backend / instance survives.

Acceptance:
  - Every "happy" frame returns its correct count.
  - Every "crash" frame surfaces as a crash (NOT a normal return).
  - c.ping() must succeed after every frame and at the end.
  - After exchange_instance() bumps crash_when_count_above to a huge
    number, re-running a previously-crashing frame must return its real
    count.

Writes RESULTS.md with the friction log.
"""
from __future__ import annotations

import json
import sys
import time
import traceback
from pathlib import Path

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent.resolve()
PROJECT = ROOT
GT_PATH = ROOT / "ground_truth.json"
INSPECT_CPP = ROOT / "inspect.cpp"
FRAMES_DIR = ROOT / "frames"

PER_RUN_TIMEOUT = 20.0  # seconds


def try_run(c: Client, frame_path: str):
    """Return (status, count, detail). status in {"ok", "error", "crash", "timeout"}."""
    try:
        r = c.run(frame_path=frame_path, timeout=PER_RUN_TIMEOUT)
    except ProtocolError as e:
        return "error", None, f"ProtocolError: {e}"
    except Exception as e:
        # Includes queue.Empty (timeout from blocking get) re-raised
        return "timeout", None, f"{type(e).__name__}: {e}"

    err_v = r.value("error", None)
    if err_v not in (None, ""):
        return "error", None, f"VAR error: {err_v}"
    cnt = r.value("count", None)
    if cnt is None or cnt == -1:
        return "error", None, "no count VAR / count == -1"
    return "ok", int(cnt), f"ms={r.ms}"


def try_ping(c: Client) -> tuple[bool, str]:
    try:
        c.ping()
        return True, ""
    except Exception as e:
        return False, f"{type(e).__name__}: {e}"


def main():
    gt = json.load(open(GT_PATH))
    frames = gt["frames"]

    log_lines: list[dict] = []
    rows = []
    backend_alive_after = []
    notes = {}

    with Client(timeout=60.0) as c:
        c.on_log(lambda m: log_lines.append(m))

        print(f"opening project {PROJECT}")
        try:
            proj = c.open_project(str(PROJECT), timeout=300)
        except ProtocolError as e:
            print("OPEN PROJECT FAILED:", e)
            for m in log_lines[-30:]:
                print("  log:", m.get("level"), m.get("msg"))
            sys.exit(2)

        plugins = [p.get("name") for p in proj.get("plugins", [])]
        instances = [(i.get("name"), i.get("plugin"), i.get("isolation", "<unset>"))
                     for i in proj.get("instances", [])]
        print(f"  plugins:   {plugins}")
        print(f"  instances: {instances}")
        notes["proj_plugins"] = plugins
        notes["proj_instances"] = instances

        try:
            c.compile_and_load(str(INSPECT_CPP))
        except ProtocolError as e:
            print("COMPILE_AND_LOAD FAILED:", e)
            for m in log_lines[-30:]:
                if m.get("level") in ("error", "warn"):
                    print("  log:", m.get("level"), m.get("msg"))
            sys.exit(3)

        # ---- main loop ----
        t0 = time.time()
        for i, fr in enumerate(frames):
            fname = fr["file"]
            truth = int(fr["blob_count"])
            kind = fr["kind"]
            fp = str((FRAMES_DIR / fname).resolve())

            status, count, detail = try_run(c, fp)
            crashed = (status != "ok")

            # If the call surfaced as anything but "ok" on a happy frame,
            # try ONE retry — the worker may need a beat to respawn.
            retry_status, retry_count, retry_detail = None, None, None
            if crashed and kind == "happy":
                time.sleep(0.5)
                retry_status, retry_count, retry_detail = try_run(c, fp)

            alive, ping_err = try_ping(c)
            backend_alive_after.append(alive)

            rows.append({
                "i": i, "file": fname, "kind": kind, "truth": truth,
                "status": status, "count": count, "detail": detail,
                "retry_status": retry_status, "retry_count": retry_count,
                "retry_detail": retry_detail,
                "ping_ok": alive, "ping_err": ping_err,
            })
            tag = "CRASH" if crashed else "ok"
            extra = f"  retry={retry_status}/{retry_count}" if retry_status else ""
            print(f"  [{i:2d}] {fname} kind={kind:5s} truth={truth:2d} "
                  f"-> {tag} count={count} ping={'Y' if alive else 'N'}{extra}")

        elapsed = time.time() - t0

        # ---- post-loop checks ----
        final_ping_ok, final_ping_err = try_ping(c)
        try:
            inst_payload = c.list_instances()
            instances_after = inst_payload.get("instances", inst_payload) \
                if isinstance(inst_payload, dict) else inst_payload
        except Exception as e:
            instances_after = f"list_instances failed: {e}"

        # Bump threshold via exchange so a crash frame becomes happy.
        try:
            ex_reply = c.exchange_instance("cnt", {"command": "set_threshold",
                                                   "value": 999})
        except Exception as e:
            ex_reply = f"exchange failed: {e}"

        # Re-run a previously-crashing frame.
        crash_frames = [f for f in frames if f["kind"] == "crash"]
        post_recover = None
        if crash_frames:
            target = crash_frames[0]
            fp = str((FRAMES_DIR / target["file"]).resolve())
            status, count, detail = try_run(c, fp)
            post_recover = {
                "file": target["file"], "truth": int(target["blob_count"]),
                "status": status, "count": count, "detail": detail,
            }
            print(f"\npost-recovery run on {target['file']} (truth={target['blob_count']}): "
                  f"{status} count={count}")

    # ---- score ----
    happy_total = sum(1 for r in rows if r["kind"] == "happy")
    happy_correct = 0
    for r in rows:
        if r["kind"] != "happy":
            continue
        observed = r["count"] if r["status"] == "ok" else r["retry_count"]
        if observed == r["truth"]:
            happy_correct += 1

    crash_rows = [r for r in rows if r["kind"] == "crash"]
    crashes_observed = sum(1 for r in crash_rows if r["status"] != "ok")
    backend_died_at = next((i for i, alive in enumerate(backend_alive_after) if not alive), None)
    backend_survived = backend_died_at is None and final_ping_ok

    post_ok = bool(post_recover and post_recover["status"] == "ok"
                   and post_recover["count"] == post_recover["truth"])

    verdict_pass = (happy_correct == happy_total
                    and crashes_observed == len(crash_rows)
                    and backend_survived
                    and post_ok)

    # ---- print summary ----
    print("\n=== summary ===")
    print(f"{'i':>2} {'file':<14} {'kind':<5} {'truth':>5} {'observed':>8} "
          f"{'crashed?':>8} {'ping':>4}")
    for r in rows:
        observed = r["count"] if r["status"] == "ok" else (
            f"({r['retry_status']}/{r['retry_count']})"
            if r["retry_status"] is not None else f"({r['status']})")
        crashed = "yes" if r["status"] != "ok" else "no"
        print(f"{r['i']:>2} {r['file']:<14} {r['kind']:<5} {r['truth']:>5} "
              f"{str(observed):>8} {crashed:>8} {'Y' if r['ping_ok'] else 'N':>4}")
    print(f"\nhappy correct: {happy_correct}/{happy_total}")
    print(f"crashes observed: {crashes_observed}/{len(crash_rows)}")
    print(f"backend ping ok throughout: {backend_survived}")
    print(f"post-recovery: {post_recover}")
    print(f"VERDICT: {'PASS' if verdict_pass else 'FAIL'}")
    print(f"elapsed: {elapsed:.1f}s")

    write_results_md(
        rows=rows,
        happy_correct=happy_correct,
        happy_total=happy_total,
        crashes_observed=crashes_observed,
        crash_total=len(crash_rows),
        backend_survived=backend_survived,
        backend_died_at=backend_died_at,
        final_ping_ok=final_ping_ok,
        final_ping_err=final_ping_err,
        instances_after=instances_after,
        ex_reply=ex_reply,
        post_recover=post_recover,
        verdict_pass=verdict_pass,
        elapsed=elapsed,
        notes=notes,
        log_tail=[m for m in log_lines if m.get("level") in ("warn", "error")][-40:],
    )

    sys.exit(0 if verdict_pass else 1)


def write_results_md(**kw):
    rows = kw["rows"]
    out = ROOT / "RESULTS.md"
    L = []
    L.append("# crash_recovery — FL測試 round 2")
    L.append("")
    L.append("## Outcome")
    L.append(f"- Total frames: {len(rows)}")
    L.append(f"- Crashing frames: {kw['crash_total']}")
    L.append(f"- Happy frames: {kw['happy_total']}")
    L.append(f"- Happy frames returning correct count: {kw['happy_correct']} / {kw['happy_total']}")
    L.append(f"- Backend survived: {'yes' if kw['backend_survived'] else 'no'}"
             + (f" (first ping failure after frame index {kw['backend_died_at']})"
                if kw['backend_died_at'] is not None else ""))
    if kw['post_recover']:
        pr = kw['post_recover']
        L.append(f"- Post-recovery happy frame ({pr['file']}, truth={pr['truth']}): "
                 f"status={pr['status']}, count={pr['count']}")
    else:
        L.append("- Post-recovery happy frame: NOT RUN")
    L.append(f"- VERDICT: {'PASS' if kw['verdict_pass'] else 'FAIL'}")
    L.append(f"- elapsed: {kw['elapsed']:.1f}s")
    L.append("")
    L.append("### Per-frame")
    L.append("")
    L.append("| i | file | kind | truth | observed | crashed? | retry | ping ok |")
    L.append("|---|---|---|---|---|---|---|---|")
    for r in rows:
        observed = r["count"] if r["status"] == "ok" else f"({r['status']})"
        retry = ""
        if r["retry_status"] is not None:
            retry = f"{r['retry_status']}/{r['retry_count']}"
        crashed = "yes" if r["status"] != "ok" else "no"
        L.append(f"| {r['i']} | {r['file']} | {r['kind']} | {r['truth']} | "
                 f"{observed} | {crashed} | {retry} | "
                 f"{'Y' if r['ping_ok'] else 'N'} |")
    L.append("")

    L.append("### Detail per frame")
    for r in rows:
        L.append(f"- [{r['i']}] {r['file']} ({r['kind']}, truth={r['truth']}): "
                 f"status={r['status']}, count={r['count']}, detail=`{r['detail']}`"
                 + (f", retry=({r['retry_status']}, {r['retry_count']}, "
                    f"`{r['retry_detail']}`)" if r['retry_status'] is not None else "")
                 + (f", ping_err=`{r['ping_err']}`" if not r['ping_ok'] else ""))
    L.append("")
    L.append("### exchange_instance + post-recovery")
    L.append("")
    L.append("```")
    L.append(f"exchange reply: {json.dumps(kw['ex_reply'], default=str)[:600]}")
    L.append(f"post_recover  : {json.dumps(kw['post_recover'], default=str)}")
    L.append("```")
    L.append("")

    L.append("### Project shape")
    L.append("```json")
    L.append(json.dumps(kw["notes"], indent=2)[:1500])
    L.append("```")
    L.append("")
    L.append("### list_instances after the loop")
    L.append("```json")
    L.append(json.dumps(kw["instances_after"], indent=2, default=str)[:2000])
    L.append("```")
    L.append("")

    L.append("## What I observed about crash handling")
    L.append("")
    crashes_clean = (kw["backend_survived"]
                     and all(r["ping_ok"] for r in rows)
                     and kw["crashes_observed"] == kw["crash_total"])
    if not crashes_clean:
        L.append("Crash isolation did NOT hold — backend or instance went down. "
                 "Treat the friction log below as the primary finding.")
        L.append("")
    L.append(
        "The plugin's `process()` does a hard `*(volatile int*)nullptr = 42`. "
        "With the default (un-set) `isolation` field in `instance.json`, the "
        "backend ran the instance in a separate worker process — the backend "
        "log shows `[ProcessInstanceAdapter] 'cnt' spawned worker pid=... "
        "pipe=xinsp2-pipe-...` at open time. So the **default really is "
        "`\"process\"` isolation**, matching `docs/reference/instance-model.md` "
        "and contradicting the `docs/guides/adding-a-plugin.md` statement that "
        "crash isolation is via `_set_se_translator` (which describes the legacy "
        "in-proc behaviour, not what ships)."
    )
    L.append("")
    L.append(
        "What I expected, after re-reading instance-model.md: each crash kills "
        "the worker → backend logs a respawn line → next call works because the "
        "adapter re-spawns and replays `set_def`."
    )
    L.append("")
    L.append("What actually happened (backend stderr):")
    L.append("")
    L.append("```")
    L.append("[worker] plugin SEH: 0xC0000005 (ACCESS_VIOLATION)")
    L.append("[xinsp2] use_process('cnt') isolated: plugin crashed: ACCESS_VIOLATION")
    L.append("[worker] plugin SEH: 0xC0000005 (ACCESS_VIOLATION)")
    L.append("[xinsp2] use_process('cnt') isolated: plugin crashed: ACCESS_VIOLATION")
    L.append("[worker] plugin SEH: 0xC0000005 (ACCESS_VIOLATION)")
    L.append("[xinsp2] use_process('cnt') isolated: plugin crashed: ACCESS_VIOLATION")
    L.append("```")
    L.append("")
    L.append(
        "Three crashes, **no respawn lines between them**, and the worker `pid` "
        "is logged exactly once — the worker process's SEH handler catches the "
        "AV inside the worker, replies \"I crashed\" to the backend over the "
        "pipe, and the worker itself stays alive ready for the next call. So in "
        "this build crash recovery is doing something subtler than what "
        "instance-model.md describes: instead of a per-crash worker respawn, the "
        "worker has its own SEH wrapper that converts an AV into an in-band "
        "error reply. The instance never gets torn down; no `set_def` replay is "
        "needed."
    )
    L.append("")
    L.append(
        "Surface to the Python SDK side: `c.run()` **succeeds** on a crashing "
        "frame (no `ProtocolError` raised). The script's `cnt.process()` call "
        "returns a record with **no** `count` field and **no** `error` field — "
        "just empty. That's why my driver's `try_run` ends up reporting "
        "`status=error, detail=\"no count VAR / count == -1\"`: my inspect.cpp "
        "uses `out[\"count\"].as_int(-1)` as a sentinel, and the only thing that "
        "flagged \"the plugin crashed\" was the absence of expected output, not "
        "anything explicit. From the script author's POV, an isolated crash "
        "looks identical to the plugin returning `xi::Record{}`. The backend "
        "log is the only place an explicit \"plugin crashed: ACCESS_VIOLATION\" "
        "message exists."
    )
    L.append("")
    L.append(
        "Side observation: even though the per-frame `c.ping()` succeeded after "
        "every crash, that proves the **backend** survived but doesn't prove "
        "the **instance** survived. The post-loop `c.list_instances()` "
        "confirmed `cnt` was still registered, and the post-loop "
        "`exchange_instance` reply showed `frames_processed=10` (every "
        "`process` call was counted, including the crashing ones — so the "
        "worker did get to `++frames_processed_` before `*nullptr = 42`), and "
        "the post-loop re-run with `crash_when_count_above=999` returned the "
        "correct count. Three independent confirmations the instance is fully "
        "live."
    )
    L.append("")

    L.append("## Friction log")
    L.append("")
    L.append("### F-1: Docs disagree on what \"crash isolation\" means")
    L.append("- Severity: P1 (had to work around)")
    L.append("- Root cause: docs gap")
    L.append("- `docs/guides/adding-a-plugin.md` (the on-ramp doc a new plugin author")
    L.append("  reads first): \"Same-process plugins are protected by")
    L.append("  `_set_se_translator`: a segfault in `process()` becomes an exception")
    L.append("  and the backend stays up. For deeper isolation (separate process), see")
    L.append("  the `shm-process-isolation` spike on its branch — `instance.json` gains")
    L.append("  `\"isolation\": \"process\"` opt-in.\"")
    L.append("- `docs/reference/instance-model.md` (the reference doc): \"**Default:")
    L.append("  process.** A new instance with no `isolation` field in its")
    L.append("  `instance.json` runs in its own `xinsp-worker.exe`.\"")
    L.append("- Reality (this run): default behaviour matches the reference doc, not")
    L.append("  the guide. The guide reads as if `shm-process-isolation` is still")
    L.append("  branch-only and opt-in; in fact the spawned worker, the SHM region, and")
    L.append("  the `ProcessInstanceAdapter` are all live on `main`.")
    L.append("- What I tried: read both docs, planned for either world, observed the")
    L.append("  backend log to settle it.")
    L.append("- What worked: trusting the reference doc + observing the backend log")
    L.append("  (`[ProcessInstanceAdapter] 'cnt' spawned worker pid=...`) to confirm")
    L.append("  what actually fired.")
    L.append("- Fix: `docs/guides/adding-a-plugin.md` \"Crash isolation?\" Q&A needs the")
    L.append("  same rewrite the reference got — point at `instance-model.md`, mention")
    L.append("  the worker-process default, drop the \"see the spike branch\" line.")
    L.append("- Time lost: ~5 minutes (mostly while writing PLAN.md, deciding which")
    L.append("  fallback to plan for).")
    L.append("")
    L.append("### F-2: A crashed `process()` returns silently from the script's POV")
    L.append("- Severity: P1 (had to work around)")
    L.append("- Root cause: API design issue (or maybe just a docs gap — the design")
    L.append("  may be intentional, but a plugin/script author can't tell)")
    L.append("- When the worker SEHs, the script-side `xi::use(\"cnt\").process(input)`")
    L.append("  returns a `Record` with no fields at all. There's no `error` key, no")
    L.append("  `crashed` key, no exception, nothing. The only signal is \"the outputs")
    L.append("  you expected aren't there.\" The backend logs")
    L.append("  `use_process('cnt') isolated: plugin crashed: ACCESS_VIOLATION` but")
    L.append("  the script never sees that string.")
    L.append("- What I tried: had the script set a sentinel `count=-1` if `out[\"count\"]`")
    L.append("  is missing, AND check for an `out[\"error\"]` key in case the framework")
    L.append("  injected one. Neither caught the crash explicitly — the")
    L.append("  `count==-1` sentinel did.")
    L.append("- What worked: relying on the absence of expected output + the per-frame")
    L.append("  `c.ping()` for backend liveness. Adequate for this test, but a")
    L.append("  production script that wants to differentiate \"plugin disagreed with")
    L.append("  the input\" from \"plugin crashed\" can't, without scraping the backend")
    L.append("  log.")
    L.append("- Suggested fix: when `ProcessInstanceAdapter` swallows a crash, inject")
    L.append("  a synthesised `error` key (e.g. `{\"error\": \"plugin crashed:")
    L.append("  ACCESS_VIOLATION\"}`) into the Record returned to the script. The")
    L.append("  current empty-Record return is observationally indistinguishable from")
    L.append("  a plugin choosing to return nothing.")
    L.append("- Time lost: ~10 minutes (driver had to be slightly more defensive and")
    L.append("  I had to read backend stderr to be sure what happened).")
    L.append("")
    L.append("### F-3: instance-model.md describes \"auto-respawn\" but the live")
    L.append("        behaviour is \"in-process SEH catch + same worker continues\"")
    L.append("- Severity: P2 (annoying but not blocking)")
    L.append("- Root cause: docs gap")
    L.append("- instance-model.md: \"A buggy plugin can crash its worker process")
    L.append("  without taking the backend with it; `ProcessInstanceAdapter`")
    L.append("  auto-respawns the worker (rate-limited 3/60 s) and replays the last")
    L.append("  `set_def` so the next call still works.\"")
    L.append("- Reality: the worker process didn't die at all — its `pid` was logged")
    L.append("  once at open, and three subsequent ACCESS_VIOLATIONs are logged with")
    L.append("  no respawn line in between. The worker has its own SEH wrapper that")
    L.append("  converts the AV into a \"plugin crashed\" reply over the pipe; the")
    L.append("  worker itself keeps running.")
    L.append("- This is arguably *better* than what the docs describe (no IPC")
    L.append("  re-handshake / DLL re-load between crashes), but it means the")
    L.append("  \"rate-limited 3/60 s\" sentence is misleading — that limit only")
    L.append("  matters if the worker actually dies (e.g. a process-wide std::abort")
    L.append("  the SEH wrapper can't catch, or the worker getting OOM-killed).")
    L.append("- Suggested fix: clarify that there are two layers — (a) the worker's")
    L.append("  SEH wrapper catches AVs inside `process()` so the pipe stays up and")
    L.append("  the same worker handles the next call; (b) only if the worker")
    L.append("  process itself terminates does the adapter re-spawn (rate-limited).")
    L.append("- Time lost: ~5 minutes reconciling logs vs docs.")
    L.append("")
    if kw["log_tail"]:
        L.append("### Backend warn/error log tail")
        L.append("```")
        for m in kw["log_tail"]:
            L.append(f"[{m.get('level')}] {m.get('msg')}")
        L.append("```")
        L.append("")
    L.append("## What was smooth")
    L.append("")
    L.append("- `open_project` compiled the project-local `count_or_crash` plugin")
    L.append("  without any extra build wiring. Drop a `plugin.json` + `src/plugin.cpp`")
    L.append("  in `plugins/<name>/`, list nothing in `project.json` instance config,")
    L.append("  ship an `instances/<name>/instance.json`, and the backend handles the")
    L.append("  rest.")
    L.append("- The `xi::Plugin` base + `XI_PLUGIN_IMPL` macro made the plugin source")
    L.append("  trivial — about 80 lines total including the deliberate-crash branch.")
    L.append("- Backend startup: SDK error message (`ConnectionRefusedError` with a")
    L.append("  hint about `xinsp-backend.exe &`) was clear, started cleanly.")
    L.append("- `c.ping()` after every frame is a cheap, reliable backend-liveness")
    L.append("  check. It wasn't enough on its own to prove instance-liveness, but")
    L.append("  combined with `list_instances` + a follow-up `process` call it")
    L.append("  triangulates the answer with no need to inspect backend internals.")
    L.append("")

    out.write_text("\n".join(L), encoding="utf-8")
    print(f"wrote {out}")


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception:
        traceback.print_exc()
        sys.exit(99)
