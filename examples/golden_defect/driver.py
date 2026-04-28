"""Drive the golden_defect xInsp2 project end-to-end.

Steps:
  1. Open the project (compiles the 3 project plugins, loads instances).
  2. Compile inspect.cpp.
  3. For each of 20 frames: set frame_idx, run, collect predictions.
  4. Score against ground_truth.json. Compute IOU on TP frames.

Usage:
    python driver.py
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent
PROJECT = ROOT
GT_PATH = ROOT / "ground_truth.json"
INSPECT_CPP = ROOT / "inspect.cpp"


def iou(a, b):
    if a is None or b is None:
        return 0.0
    ax0, ay0, ax1, ay1 = a
    bx0, by0, bx1, by1 = b
    ix0, iy0 = max(ax0, bx0), max(ay0, by0)
    ix1, iy1 = min(ax1, bx1), min(ay1, by1)
    iw, ih = max(0, ix1 - ix0 + 1), max(0, iy1 - iy0 + 1)
    inter = iw * ih
    if inter == 0:
        return 0.0
    aa = (ax1 - ax0 + 1) * (ay1 - ay0 + 1)
    bb = (bx1 - bx0 + 1) * (by1 - by0 + 1)
    return inter / (aa + bb - inter)


def centroid_dist(a, b):
    if a is None or b is None:
        return None
    acx = (a[0] + a[2]) / 2; acy = (a[1] + a[3]) / 2
    bcx = (b[0] + b[2]) / 2; bcy = (b[1] + b[3]) / 2
    return ((acx - bcx) ** 2 + (acy - bcy) ** 2) ** 0.5


def run_all():
    with open(GT_PATH) as f:
        gt = json.load(f)

    rows = []
    logs = []
    with Client() as c:
        def _log(m):
            logs.append(m)
            if m.get("level") in ("warn", "error"):
                print("  log:", m["level"], m["msg"][:200])
        c.on_log(_log)
        print(f"opening project {PROJECT}")
        try:
            c.open_project(str(PROJECT), timeout=600)
        except ProtocolError as e:
            print("OPEN FAILED:", e)
            for m in logs[-30:]:
                print(" ", m.get("level"), m.get("msg"))
            return None

        try:
            c.compile_and_load(str(INSPECT_CPP))
        except ProtocolError as e:
            print("COMPILE FAILED:", e)
            for m in logs[-30:]:
                if m.get("level") in ("error", "warn"):
                    print(" ", m["level"], m["msg"])
            return None

        # Sanity: ask the reference plugin about its state.
        try:
            ref_status = c.call("exchange_instance",
                                {"name": "ref", "cmd": {"command": "get_status"}})
            print("  reference status:", ref_status)
        except ProtocolError as e:
            print("  reference status query failed:", e)

        for fr in gt["frames"]:
            idx = int(fr["file"].split("_")[1].split(".")[0])
            c.set_param("frame_idx", idx)
            run = c.run()
            if idx == 0:
                print("  vars on first run:", [v["name"] for v in run.vars])
                err = run.value("error")
                if err:
                    print("  error VAR:", err)
            pred = run.value("defect_present")
            score = run.value("score") or 0.0
            la = run.value("largest_area") or 0
            bb = (run.value("bbox_x0"), run.value("bbox_y0"),
                  run.value("bbox_x1"), run.value("bbox_y1"))
            if bb[0] is None or bb[0] < 0:
                bb = None
            rows.append({
                "file":       fr["file"],
                "truth":      bool(fr["defect"]),
                "truth_type": fr["type"],
                "truth_bbox": fr["bbox"],
                "pred":       bool(pred) if pred is not None else False,
                "score":      score,
                "largest":    la,
                "pred_bbox":  list(bb) if bb else None,
            })
    return rows


def score(rows):
    tp = sum(1 for r in rows if r["truth"] and r["pred"])
    tn = sum(1 for r in rows if not r["truth"] and not r["pred"])
    fp = sum(1 for r in rows if not r["truth"] and r["pred"])
    fn = sum(1 for r in rows if r["truth"] and not r["pred"])
    correct = tp + tn
    n = len(rows)

    ious, dists = [], []
    for r in rows:
        if r["truth"] and r["pred"] and r["truth_bbox"] and r["pred_bbox"]:
            ious.append(iou(r["truth_bbox"], r["pred_bbox"]))
            dists.append(centroid_dist(r["truth_bbox"], r["pred_bbox"]))

    by_type = {}
    for r in rows:
        if r["truth"]:
            t = r["truth_type"]
            d = by_type.setdefault(t, {"hit": 0, "n": 0})
            d["n"] += 1
            if r["pred"]:
                d["hit"] += 1

    return {
        "n": n, "correct": correct, "tp": tp, "tn": tn, "fp": fp, "fn": fn,
        "mean_iou": sum(ious)/len(ious) if ious else None,
        "mean_centroid_dist": sum(dists)/len(dists) if dists else None,
        "by_type": by_type,
    }


def print_results(rows, sc):
    print(f"\n{'frame':<14}{'type':<10}{'truth':>6}{'pred':>6}{'score':>9}{'largest':>9}  {'truth_bbox':<22}  pred_bbox")
    print("-" * 110)
    for r in rows:
        tb = str(r["truth_bbox"]) if r["truth_bbox"] else "—"
        pb = str(r["pred_bbox"])  if r["pred_bbox"]  else "—"
        sv = r['score']
        s  = f"{sv:.2f}" if isinstance(sv, (int, float)) else "—"
        la_s = str(r['largest']) if r['largest'] is not None else "—"
        print(f"{r['file']:<14}{r['truth_type']:<10}{str(r['truth'])[0]:>6}{str(r['pred'])[0]:>6}"
              f"{s:>9}{la_s:>9}  {tb:<22}  {pb}")
    print("-" * 110)
    print(f"correct {sc['correct']}/{sc['n']}   TP={sc['tp']} TN={sc['tn']} FP={sc['fp']} FN={sc['fn']}")
    if sc['mean_iou'] is not None:
        print(f"mean IOU on TP frames: {sc['mean_iou']:.3f}    mean centroid dist: {sc['mean_centroid_dist']:.1f} px")
    print(f"per-type recall: {sc['by_type']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="write rows + score as JSON")
    args = ap.parse_args()
    rows = run_all()
    if rows is None:
        sys.exit(1)
    sc = score(rows)
    print_results(rows, sc)
    if args.out:
        Path(args.out).write_text(json.dumps({"rows": rows, "score": sc}, indent=2))
        print(f"wrote {args.out}")
    # acceptance gate
    ok = sc["correct"] >= 18 and sc["fp"] <= 1 and sc["fn"] <= 1
    print("ACCEPTANCE:", "PASS" if ok else "FAIL")


if __name__ == "__main__":
    main()
