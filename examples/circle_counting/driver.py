"""Driver: convert PNGs to raw, compile, run all 20 frames, score.

Usage:
    python driver.py            # full evaluation
    python driver.py --sweep    # parameter sweep (manual exploration)
"""
import argparse
import json
import os
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent
FRAMES_DIR = ROOT / "frames"
RAW_DIR = ROOT / "frames_raw"
GT_PATH = ROOT / "ground_truth.json"
INSPECT_CPP = ROOT / "inspect.cpp"


def convert_pngs_to_raw():
    RAW_DIR.mkdir(exist_ok=True)
    for png in sorted(FRAMES_DIR.glob("frame_*.png")):
        arr = np.array(Image.open(png).convert("L"), dtype=np.uint8)
        out = RAW_DIR / (png.stem + ".raw")
        out.write_bytes(arr.tobytes())
    print(f"converted {len(list(FRAMES_DIR.glob('frame_*.png')))} PNGs to raw in {RAW_DIR}")


def run_all(params=None):
    with open(GT_PATH) as f:
        gt = json.load(f)
    truth = {fr["file"]: fr["count"] for fr in gt["frames"]}

    results = []
    with Client() as c:
        try:
            c.compile_and_load(str(INSPECT_CPP))
        except ProtocolError as e:
            print("COMPILE FAILED:", e)
            return None
        if params:
            for k, v in params.items():
                c.set_param(k, v)
        for fr in gt["frames"]:
            idx = int(fr["file"].split("_")[1].split(".")[0])
            c.set_param("frame_idx", idx)
            run = c.run()
            pred = run.value("count")
            t = truth[fr["file"]]
            results.append((fr["file"], idx, t, pred, abs((pred or 0) - t)))
    return results


def print_results(results):
    print(f"{'frame':<14}{'truth':>7}{'pred':>7}{'err':>6}")
    print("-" * 36)
    for f, _, t, p, err in results:
        print(f"{f:<14}{t:>7}{p:>7}{err:>6}")
    n = len(results)
    avg_err = sum(r[4] for r in results) / n
    exact = sum(1 for r in results if r[4] == 0)
    print("-" * 36)
    print(f"avg abs err: {avg_err:.3f}   exact: {exact}/{n}")
    return avg_err, exact


def sweep():
    convert_pngs_to_raw()
    grid = []
    for diff_C in [10, 15, 20, 25, 30]:
        for block_radius in [20, 30, 40, 50]:
            for min_area in [200, 400, 600]:
                for max_area in [3000, 4500]:
                    params = {
                        "diff_C": diff_C,
                        "block_radius": block_radius,
                        "min_area": min_area,
                        "max_area": max_area,
                    }
                    res = run_all(params)
                    if res is None:
                        return
                    avg_err = sum(r[4] for r in res) / len(res)
                    exact = sum(1 for r in res if r[4] == 0)
                    grid.append((avg_err, exact, params))
                    print(f"params={params} -> avg_err={avg_err:.3f} exact={exact}/{len(res)}")
    grid.sort(key=lambda x: (x[0], -x[1]))
    print("\nTOP 5:")
    for row in grid[:5]:
        print(row)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sweep", action="store_true")
    ap.add_argument("--no-convert", action="store_true")
    args = ap.parse_args()
    if args.sweep:
        sweep()
        return
    if not args.no_convert:
        convert_pngs_to_raw()
    res = run_all()
    if res is None:
        sys.exit(1)
    print_results(res)


if __name__ == "__main__":
    main()
