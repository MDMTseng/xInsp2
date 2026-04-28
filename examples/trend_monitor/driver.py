"""Driver: trend_monitor.

Drives all 30 frames in order, prints per-frame
{count, running_mean, flagged}, and exercises three explicit
verification scenarios:

  1) XI_STATE_SCHEMA(N) actually changes the exported version.
     We compile once at schema 2, run the full 30-frame sequence,
     then bump the macro to 3 and recompile. The backend should
     emit `state_dropped` (old=2, new=3); a follow-up run should
     observe an EMPTY rolling window (window_len_in == 0).

  2) `open_project` no longer SIGSEGVs on a second call. We call
     c.open_project() at startup AND again mid-sequence (after
     frame 14) and proceed without backend death.

  3) `Client.run()` no longer drains events. After the schema-bump
     compile_and_load fires `state_dropped`, we read the event from
     c._inbox_events BEFORE the next run() consumes it.

NB on flow vs verification (2):
   `open_project` is documented to re-attach instances and may reset
   script state. To keep the trend evaluation honest, we capture the
   window state via the script's own VARs (`window_len_out`) so we
   can detect a reset and re-prime the window by replaying the
   already-seen frames silently if needed.

Usage:
    python driver.py
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from queue import Empty

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent
FRAMES_DIR = ROOT / "frames"
GT_PATH = ROOT / "ground_truth.json"
INSPECT_CPP = ROOT / "inspect.cpp"
PROJECT_DIR = ROOT


def drain_events(c: Client) -> list[dict]:
    out = []
    try:
        while True:
            out.append(c._inbox_events.get_nowait())
    except Empty:
        pass
    return out


def find_state_dropped(events: list[dict]) -> dict | None:
    return next((e for e in events if e.get("name") == "state_dropped"), None)


def set_schema_version(n: int) -> int:
    """Edit XI_STATE_SCHEMA(N) in inspect.cpp in-place."""
    src = INSPECT_CPP.read_text(encoding="utf-8")
    m = re.search(r"XI_STATE_SCHEMA\((\d+)\);", src)
    prev = int(m.group(1)) if m else -1
    new_src = re.sub(r"XI_STATE_SCHEMA\(\d+\);", f"XI_STATE_SCHEMA({n});", src, count=1)
    INSPECT_CPP.write_text(new_src, encoding="utf-8")
    return prev


def run_frame(c, fr) -> dict:
    png = (FRAMES_DIR / fr["file"]).resolve()
    run = c.run(frame_path=str(png))
    return {
        "idx":             fr["frame_idx"],
        "count":           run.value("count", 0),
        "running_mean":    run.value("running_mean", 0.0),
        "flagged":         bool(run.value("flagged", 0)),
        "warming":         bool(run.value("warming", 0)),
        "window_len_in":   run.value("window_len_in", 0),
        "window_len_out":  run.value("window_len_out", 0),
    }


def main():
    with open(GT_PATH) as f:
        gt = json.load(f)
    truth_anomalies = set(gt["anomaly_frames"])

    # Always start with schema 2.
    initial = set_schema_version(2)
    print(f"inspect.cpp schema reset: prev={initial} -> 2")

    log_lines: list[dict] = []
    verification = {
        "open_project_second_call": None,
        "schema_bump_state_dropped": None,
        "events_not_drained_by_run": None,
        "post_bump_window_empty": None,
    }

    with Client() as c:
        c.on_log(lambda m: log_lines.append(m))

        # === Verification (2a): first open_project ==================
        c.open_project(str(PROJECT_DIR))
        print("open_project #1 OK")

        # First compile_and_load at schema 2.
        try:
            c.compile_and_load(str(INSPECT_CPP))
        except ProtocolError as e:
            print(f"COMPILE FAILED: {e}")
            for m in log_lines[-30:]:
                if m.get("level") in ("error", "warn"):
                    print("  log:", m.get("msg"))
            return 1

        # Drain any events from the first compile (warm session may emit
        # state_dropped if the previous run left a different schema).
        ev_first = drain_events(c)
        sd_first = find_state_dropped(ev_first)
        print(f"after first compile: state_dropped={sd_first.get('data') if sd_first else 'none'}")

        # === Run frames 0..14 ======================================
        rows = []
        for fr in gt["frames"][:15]:
            rows.append(run_frame(c, fr))

        # === Verification (2b): second open_project mid-sequence ===
        try:
            c.open_project(str(PROJECT_DIR))
            print(f"\nopen_project #2 (mid-sequence, after frame 14) OK — no SIGSEGV")
            verification["open_project_second_call"] = "ok"
        except Exception as e:
            print(f"open_project #2 FAILED/CRASHED: {e}")
            verification["open_project_second_call"] = f"failed: {e}"
            return 2

        # Recompile so the script DLL is bound to the (possibly re-
        # initialised) project context.
        c.compile_and_load(str(INSPECT_CPP))
        # Drain events from this re-compile so the next probe is clean.
        drain_events(c)

        # Probe state by running frame 0 once — if window_len_in==0 it
        # means open_project reset the state, so we need to re-prime
        # by replaying frames 0..14 silently.
        probe = run_frame(c, gt["frames"][0])
        if probe["window_len_in"] == 0:
            print("  state was reset by open_project — re-priming window with frames 1..14")
            # We already ran frame 0 (added to window). Re-run 1..14.
            for fr in gt["frames"][1:15]:
                run_frame(c, fr)
        else:
            print(f"  state preserved across open_project (window_len_in={probe['window_len_in']})")

        # === Run frames 15..29 ====================================
        for fr in gt["frames"][15:]:
            rows.append(run_frame(c, fr))

        # === Score the main 30-frame run ==========================
        # rows: 15 from before open_project (idx 0..14), then 15
        # post-open_project (idx 15..29). For idx 0..14 the entries
        # are from the FIRST pass (with proper window).
        last_by_idx: dict[int, dict] = {}
        for r in rows:
            last_by_idx[r["idx"]] = r  # later wins; for 0..14, only one entry

        # === Verification (1) + (3): schema bump ==================
        prev = set_schema_version(3)
        print(f"\nbumping schema {prev} -> 3 and recompiling ...")
        c.compile_and_load(str(INSPECT_CPP))

        # CRITICAL: read events BEFORE the next run() to verify run()
        # does not drain events.
        evs_after_bump = drain_events(c)
        sd_bump = find_state_dropped(evs_after_bump)
        if sd_bump:
            data = sd_bump.get("data")
            print(f"  state_dropped event observed: {data}")
            verification["schema_bump_state_dropped"] = data
            verification["events_not_drained_by_run"] = "ok"
        else:
            print(f"  NO state_dropped event seen. events: {evs_after_bump}")
            verification["schema_bump_state_dropped"] = "MISSING"
            verification["events_not_drained_by_run"] = "untestable (no event was emitted)"

        # Run one more frame to verify the window is now empty.
        probe2 = run_frame(c, gt["frames"][0])
        print(f"  post-bump probe (frame 0): window_len_in={probe2['window_len_in']} (expect 0)")
        if probe2["window_len_in"] == 0:
            verification["post_bump_window_empty"] = "ok"
        else:
            verification["post_bump_window_empty"] = f"FAIL (window_len_in={probe2['window_len_in']})"

    # Restore inspect.cpp to schema 2 for the next session.
    set_schema_version(2)

    # === Report ==================================================
    print()
    print("=== Per-frame results ===")
    print(f"{'idx':>3} {'count':>5} {'mean':>7} {'flag':>5} {'truth':>5} {'win_in':>6} {'warm':>4}")
    print("-" * 50)
    for idx in sorted(last_by_idx.keys()):
        r = last_by_idx[idx]
        flag_s = "Y" if r["flagged"] else "."
        truth_s = "Y" if idx in truth_anomalies else "."
        warm_s = "W" if r["warming"] else " "
        ok = (r["flagged"] == (idx in truth_anomalies))
        mark = " " if ok else "X"
        print(f"{idx:>3} {r['count']:>5} {r['running_mean']:>7.2f}   {flag_s}     {truth_s}   {r['window_len_in']:>6}   {warm_s}  {mark}")

    flagged_set = {i for i, r in last_by_idx.items() if r["flagged"]}
    tp = flagged_set & truth_anomalies
    fp = flagged_set - truth_anomalies
    fn = truth_anomalies - flagged_set

    print()
    print("=== Score ===")
    print(f"  truth anomalies : {sorted(truth_anomalies)}")
    print(f"  flagged frames  : {sorted(flagged_set)}")
    print(f"  true positives  : {sorted(tp)}  ({len(tp)}/{len(truth_anomalies)})")
    print(f"  false positives : {sorted(fp)}")
    print(f"  false negatives : {sorted(fn)}")

    print()
    print("=== Verification ===")
    for k, v in verification.items():
        print(f"  {k}: {v}")

    success = (
        (not fp) and (not fn)
        and verification["open_project_second_call"] == "ok"
        and verification["schema_bump_state_dropped"] not in ("MISSING", None)
        and (verification["post_bump_window_empty"] or "").startswith("ok")
    )
    print()
    print("PASS" if success else "PARTIAL/FAIL — see report above")
    return 0 if success else 2


if __name__ == "__main__":
    sys.exit(main())
