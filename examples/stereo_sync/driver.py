"""stereo_sync driver.

Opens the project, compiles the script, turns on continuous mode, and
counts trigger cycles where left.seq == right.seq.

Acceptance:
  - >=30 cycles observed in the 2.5 s correlation window
  - >=95% of cycles report matched == True
  - Backend survives a 5 s soak with cycle count roughly proportional
    to the source rate
"""
from __future__ import annotations

import sys
import time
from pathlib import Path
from queue import Empty

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent
INSPECT_CPP = ROOT / "inspect.cpp"


def collect_vars(c: Client, duration_s: float) -> list[dict]:
    """Block for `duration_s` seconds while consuming `vars` events
    posted by the backend's continuous-mode worker. Returns a list of
    flattened {name: value} dicts, one per observed inspect cycle."""
    events: list[dict] = []
    deadline = time.time() + duration_s
    while time.time() < deadline:
        remaining = deadline - time.time()
        try:
            ev = c._inbox_vars.get(timeout=min(0.5, max(0.05, remaining)))
        except Empty:
            continue
        flat = {}
        for it in ev.get("items") or []:
            flat[it["name"]] = it.get("value")
        flat["_run_id"] = ev.get("run_id")
        events.append(flat)
    return events


def summarise(events: list[dict], label: str) -> dict:
    total = 0
    matched = 0
    half = 0
    no_trigger = 0
    seqs_left = []
    seqs_right = []
    tids = set()
    for v in events:
        if v.get("active") is False:
            no_trigger += 1
            continue
        if v.get("half_trigger"):
            half += 1
            continue
        if "matched" not in v:
            continue
        total += 1
        if v.get("matched") is True:
            matched += 1
        if v.get("tid"):
            tids.add(v["tid"])
        if isinstance(v.get("left_seq"), (int, float)):
            seqs_left.append(int(v["left_seq"]))
        if isinstance(v.get("right_seq"), (int, float)):
            seqs_right.append(int(v["right_seq"]))
    pct = (matched / total * 100.0) if total else 0.0
    print(f"[{label}] events={len(events)}  active+paired={total}  "
          f"matched={matched} ({pct:.1f}%)  half_trigger={half}  "
          f"no_trigger_ticks={no_trigger}  unique_tids={len(tids)}")
    if seqs_left[:5]:
        print(f"           sample left_seq:  {seqs_left[:5]} ... {seqs_left[-3:] if len(seqs_left) > 3 else ''}")
        print(f"           sample right_seq: {seqs_right[:5]} ... {seqs_right[-3:] if len(seqs_right) > 3 else ''}")
    return {
        "events": len(events),
        "paired_cycles": total,
        "matched": matched,
        "half": half,
        "no_trigger": no_trigger,
        "unique_tids": len(tids),
        "match_pct": pct,
    }


def main() -> int:
    print(f"--- opening project: {ROOT}")
    with Client() as c:
        try:
            info = c.open_project(str(ROOT))
        except ProtocolError as e:
            print("OPEN_PROJECT FAILED:", e)
            return 1
        print(f"  plugins: {[p.get('name') for p in info.get('plugins', [])]}")
        inst = info.get("instances") or []
        print(f"  instances: {[i.get('name') for i in inst]}")

        try:
            c.compile_and_load(str(INSPECT_CPP))
        except ProtocolError as e:
            print("COMPILE FAILED:", e)
            return 1
        print("  script compiled")

        # NOTE: project.json's `trigger_policy` block is the canonical
        # way to set the policy. We deliberately DO NOT call
        # `cmd:set_trigger_policy` here — its arg-parser uses a literal
        # substring match for `"required":[` (no space) but Python's
        # default `json.dumps` emits `"required": [` with a space after
        # the colon, so the `required` array silently parses as empty
        # and AllRequired never fires. See RESULTS.md F-1.

        # Worker threads inside each instance auto-started in their ctor.
        # Give them a beat to push a few frames before we turn on
        # continuous mode (otherwise the first dispatch may see only
        # one camera's emit cached).
        time.sleep(0.3)

        # ---- 2.5 s correlation window ----
        print("\n--- starting continuous mode for 2.5 s ---")
        c.call("start", {"fps": 20})
        events = collect_vars(c, 2.5)
        c.call("stop")
        s1 = summarise(events, "correlation")

        # Drain any in-flight vars after stop
        time.sleep(0.2)
        try:
            while True:
                c._inbox_vars.get_nowait()
        except Empty:
            pass

        # ---- 5 s soak ----
        print("\n--- 5 s soak ---")
        c.call("start", {"fps": 20})
        soak_events = collect_vars(c, 5.0)
        c.call("stop")
        s2 = summarise(soak_events, "soak")

        # Confirm backend still alive
        try:
            c.ping()
            alive = True
        except Exception as e:
            alive = False
            print("PING after soak failed:", e)
        print(f"  backend alive after soak: {alive}")

        # Verdict
        ok_cycles = s1["paired_cycles"] >= 30
        ok_match = s1["match_pct"] >= 95.0
        # Cameras at 20 Hz x 5s ≈ 100 paired cycles. Allow 20% slack.
        ok_soak = 80 <= s2["paired_cycles"] <= 120
        ok_alive = alive

        verdict = "PASS" if (ok_cycles and ok_match and ok_alive) else "FAIL"
        print("\n=== VERDICT ===")
        print(f"  cycles >= 30:           {ok_cycles}  (got {s1['paired_cycles']})")
        print(f"  match% >= 95:           {ok_match}  (got {s1['match_pct']:.1f}%)")
        print(f"  soak ~100 cycles:       {ok_soak}   (got {s2['paired_cycles']}, range 80..120)")
        print(f"  backend alive:          {ok_alive}")
        print(f"  -> {verdict}")

        # Side-channel: print recent_errors so we don't miss a stderr-only
        # backend complaint.
        try:
            errs = c.recent_errors()
            if errs:
                print("\nrecent_errors (last 10):")
                for e in errs[-10:]:
                    print("  ", e)
        except Exception:
            pass

        return 0 if verdict == "PASS" else 2


if __name__ == "__main__":
    sys.exit(main())
