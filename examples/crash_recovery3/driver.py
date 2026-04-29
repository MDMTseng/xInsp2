"""Driver for crash_recovery3.

Procedure (per spec):
  1. open_project + compile_and_load.
  2. Loop the 10 frames; record (count, error, crashed, ping_ok).
  3. After loop: c.ping(), c.list_instances(), exchange threshold high,
     final happy run on a previously-crashing frame.
  4. Print summary; assert acceptance criteria.
"""
import json
import sys
import time
from pathlib import Path

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent
GT_PATH = ROOT / "ground_truth.json"
INSPECT_CPP = ROOT / "inspect.cpp"
FRAMES_DIR = ROOT / "frames"


def fpath(name: str) -> str:
    # Forward slashes for the JSON wire.
    return str((FRAMES_DIR / name).resolve()).replace("\\", "/")


def main() -> int:
    gt = json.loads(GT_PATH.read_text())
    frames = gt["frames"]
    default_threshold = gt["threshold_default"]

    rows = []  # (file, truth, observed, crashed, error, ping_ok_after)

    with Client() as c:
        try:
            c.open_project(str(ROOT).replace("\\", "/"))
        except ProtocolError as e:
            print("OPEN_PROJECT FAILED:", e)
            return 2
        try:
            c.compile_and_load(str(INSPECT_CPP).replace("\\", "/"))
        except ProtocolError as e:
            print("COMPILE FAILED:", e)
            return 2

        for fr in frames:
            name = fr["file"]
            truth = fr["blob_count"]
            t0 = time.time()
            try:
                run = c.run(frame_path=fpath(name), timeout=20.0)
                count = run.value("count", -1)
                err = run.value("error", "") or ""
                crashed_var = run.value("crashed", False)
                run_exc = None
            except Exception as e:
                count = -1
                err = f"<run-raised: {e}>"
                crashed_var = True
                run_exc = e
            ms = int((time.time() - t0) * 1000)

            # Verify backend survived.
            ping_ok = True
            try:
                c.ping()
            except Exception as e:
                ping_ok = False
                err = (err + f" | ping_after_failed: {e}").strip()

            rows.append({
                "file": name,
                "truth": truth,
                "observed": count,
                "crashed": bool(crashed_var) or run_exc is not None,
                "error": err,
                "ping_ok_after": ping_ok,
                "ms": ms,
            })

        # Post-loop checks
        backend_alive = True
        try:
            c.ping()
        except Exception as e:
            backend_alive = False
            print("FINAL ping failed:", e)

        try:
            inst_payload = c.list_instances()
        except Exception as e:
            print("list_instances failed:", e)
            inst_payload = {}
        instance_names = []
        if isinstance(inst_payload, dict):
            for it in inst_payload.get("items", []) or inst_payload.get("instances", []) or []:
                if isinstance(it, dict):
                    nm = it.get("name") or it.get("instance_name")
                    if nm:
                        instance_names.append(nm)
        instance_present = "counter" in instance_names

        # Bump threshold high so the previously-crashing frame is happy now.
        post_recovery_count = None
        post_recovery_ok = False
        post_recovery_file = None
        try:
            c.exchange_instance("counter", {
                "command": "set_threshold", "value": 1000,
            })
            crashing = [f for f in frames if f["blob_count"] > default_threshold]
            if crashing:
                post_recovery_file = crashing[0]["file"]
                expected = crashing[0]["blob_count"]
                run = c.run(frame_path=fpath(post_recovery_file), timeout=20.0)
                post_recovery_count = run.value("count", -1)
                post_err = run.value("error", "") or ""
                post_recovery_ok = (post_recovery_count == expected and not post_err)
                print(f"\npost-recovery run on {post_recovery_file}: "
                      f"observed={post_recovery_count} expected={expected} "
                      f"err={post_err!r}")
        except Exception as e:
            print("post-recovery step raised:", e)

    # ---- summary ----
    print()
    print(f"{'frame':<14}{'truth':>7}{'obs':>6}{'crash':>7}{'ping_ok':>9}  err")
    print("-" * 80)
    for r in rows:
        print(f"{r['file']:<14}{r['truth']:>7}{r['observed']:>6}"
              f"{str(r['crashed']):>7}{str(r['ping_ok_after']):>9}  "
              f"{r['error'][:40]}")

    happy = [r for r in rows if r["truth"] <= default_threshold]
    crashing = [r for r in rows if r["truth"] > default_threshold]
    happy_correct = sum(1 for r in happy if r["observed"] == r["truth"] and not r["crashed"])
    crashing_absorbed = sum(1 for r in crashing if r["crashed"] and r["ping_ok_after"])

    print("-" * 80)
    print(f"happy frames returning correct count: {happy_correct} / {len(happy)}")
    print(f"crashing frames absorbed (crashed flag + backend alive): "
          f"{crashing_absorbed} / {len(crashing)}")
    print(f"backend alive at end: {backend_alive}")
    print(f"instance 'counter' still listed: {instance_present}")
    print(f"post-recovery happy run ok: {post_recovery_ok}")

    verdict_pass = (
        happy_correct == len(happy)
        and crashing_absorbed == len(crashing)
        and backend_alive
        and instance_present
        and post_recovery_ok
    )
    print(f"VERDICT: {'PASS' if verdict_pass else 'FAIL'}")

    summary = {
        "rows": rows,
        "happy_correct": happy_correct,
        "happy_total": len(happy),
        "crashing_absorbed": crashing_absorbed,
        "crashing_total": len(crashing),
        "backend_alive": backend_alive,
        "instance_present": instance_present,
        "post_recovery_ok": post_recovery_ok,
        "post_recovery_count": post_recovery_count,
        "post_recovery_file": post_recovery_file,
        "verdict_pass": verdict_pass,
    }
    (ROOT / "driver_summary.json").write_text(json.dumps(summary, indent=2))
    return 0 if verdict_pass else 1


if __name__ == "__main__":
    sys.exit(main())
