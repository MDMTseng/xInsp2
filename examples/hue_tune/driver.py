"""Driver: open hue_tune project, exercise exchange()-based live tuning.

3 sweeps over the same 8 frames:
  red   (reset)                            → expect 6
  blue  (set_hue_range lo=110 hi=130)      → expect 4
  green (set_hue_range lo=50  hi=70)       → expect 2

Pass iff all 24 frames hit their target.
"""
import json
import sys
import time
from pathlib import Path

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent.resolve()
PROJECT = ROOT
INSPECT_CPP = ROOT / "inspect.cpp"
FRAMES_DIR = ROOT / "frames"
GT_PATH = ROOT / "ground_truth.json"

INSTANCE = "det"

SWEEPS = [
    ("red",   {"command": "reset"},                                         6),
    ("blue",  {"command": "set_hue_range", "lo": 110, "hi": 130},           4),
    ("green", {"command": "set_hue_range", "lo": 50,  "hi":  70},           2),
]


def main():
    gt = json.loads(GT_PATH.read_text())
    frame_files = [fr["file"] for fr in gt["frames"]]

    log_lines = []
    with Client() as c:
        c.on_log(lambda m: log_lines.append(m))

        print(f"opening project {PROJECT}")
        try:
            proj = c.open_project(str(PROJECT))
        except ProtocolError as e:
            print("OPEN PROJECT FAILED:", e)
            for m in log_lines[-30:]:
                if m.get("level") in ("error", "warn"):
                    print("  log:", m.get("level"), m.get("msg"))
            sys.exit(2)

        plugins = [p.get("name") for p in proj.get("plugins", [])]
        instances = [(i.get("name"), i.get("plugin")) for i in proj.get("instances", [])]
        print(f"  plugins:   {plugins}")
        print(f"  instances: {instances}")

        try:
            c.compile_and_load(str(INSPECT_CPP))
        except ProtocolError as e:
            print("COMPILE FAILED:", e)
            for m in log_lines[-40:]:
                if m.get("level") == "error":
                    print("  log:", m.get("msg"))
            sys.exit(3)

        sweep_results = {}
        t0 = time.time()
        for sweep_name, exch_cmd, target in SWEEPS:
            print(f"\n--- sweep '{sweep_name}' (target={target}) ---")
            try:
                rsp = c.exchange_instance(INSTANCE, exch_cmd)
                print(f"  exchange rsp: {rsp}")
            except ProtocolError as e:
                print(f"  EXCHANGE FAILED: {e}")
                sweep_results[sweep_name] = (0, [])
                continue

            hits = 0
            per_frame = []
            for fname in frame_files:
                frame_path = str((FRAMES_DIR / fname).resolve())
                run = c.run(frame_path=frame_path, timeout=60)
                pred = run.value("count", -1)
                hlo = run.value("hue_lo", -1)
                hhi = run.value("hue_hi", -1)
                ok = (pred == target)
                hits += int(ok)
                per_frame.append({
                    "file": fname, "pred": pred,
                    "hue_lo": hlo, "hue_hi": hhi,
                    "ok": ok,
                })
                mark = "OK " if ok else "BAD"
                print(f"  {mark} {fname}: count={pred} (band={hlo}..{hhi})")
            sweep_results[sweep_name] = (hits, per_frame)
            print(f"  -> {hits}/{len(frame_files)}")
        elapsed = time.time() - t0

    n = len(frame_files)
    red   = sweep_results.get("red",   (0, []))[0]
    blue  = sweep_results.get("blue",  (0, []))[0]
    green = sweep_results.get("green", (0, []))[0]
    total = red + blue + green
    cap = 3 * n

    print("\n=== SUMMARY ===")
    print(f"red:   {red}/{n}")
    print(f"blue:  {blue}/{n}")
    print(f"green: {green}/{n}")
    print(f"total: {total}/{cap}  (elapsed {elapsed:.1f}s)")

    summary = {
        "n_frames": n,
        "red":   red,
        "blue":  blue,
        "green": green,
        "total": total,
        "cap":   cap,
        "elapsed_s": elapsed,
        "details": {k: v[1] for k, v in sweep_results.items()},
    }
    (ROOT / "driver_summary.json").write_text(json.dumps(summary, indent=2))

    sys.exit(0 if total == cap else 1)


if __name__ == "__main__":
    main()
