"""Driver v2: open the circle_counting xInsp2 project, run all 20 frames, score.

Usage:
    python driver_v2.py
"""
import argparse
import json
import sys
from pathlib import Path

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent
PROJECT = ROOT
GT_PATH = ROOT / "ground_truth.json"
INSPECT_CPP = ROOT / "inspect.cpp"


def run_all():
    with open(GT_PATH) as f:
        gt = json.load(f)
    truth = {fr["file"]: fr["count"] for fr in gt["frames"]}

    results = []
    with Client() as c:
        # Open the project — compiles the three project plugins, loads
        # instance.json for each instance.
        print(f"opening project {PROJECT}")
        proj = c.call("open_project", {"folder": str(PROJECT)}, timeout=180)
        plugins = [p.get("name") for p in proj.get("plugins", [])]
        instances = [(i.get("name"), i.get("plugin")) for i in proj.get("instances", [])]
        print(f"  plugins: {plugins}")
        print(f"  instances: {instances}")

        # Compile the inspection script.
        try:
            c.compile_and_load(str(INSPECT_CPP))
        except ProtocolError as e:
            print("COMPILE FAILED:", e)
            return None

        for fr in gt["frames"]:
            idx = int(fr["file"].split("_")[1].split(".")[0])
            c.set_param("frame_idx", idx)
            run = c.run()
            pred = run.value("count")
            t = truth[fr["file"]]
            err = abs((pred if isinstance(pred, int) else 0) - t)
            results.append((fr["file"], idx, t, pred, err))
    return results


def print_results(results):
    print(f"\n{'frame':<14}{'truth':>7}{'pred':>7}{'err':>6}")
    print("-" * 36)
    for f, _, t, p, err in results:
        print(f"{f:<14}{t:>7}{str(p):>7}{err:>6}")
    n = len(results)
    avg_err = sum(r[4] for r in results) / n
    exact = sum(1 for r in results if r[4] == 0)
    print("-" * 36)
    print(f"avg abs err: {avg_err:.3f}   exact: {exact}/{n}")
    return avg_err, exact


def main():
    ap = argparse.ArgumentParser()
    args = ap.parse_args()
    res = run_all()
    if res is None:
        sys.exit(1)
    print_results(res)


if __name__ == "__main__":
    main()
