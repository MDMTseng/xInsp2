"""Score predictions.json (from driver.py) against ground_truth.json.

Greedy nearest-centroid matching within ground_truth's tolerance_px. Reports
per-frame count accuracy and overall precision / recall / mean localization
error. Exit 0 only if every object is matched (count exact on all frames,
no false positives or misses).
"""
from __future__ import annotations
import json, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
gt_path = ROOT / "ground_truth.json"
pred_path = ROOT / "predictions.json"
if not gt_path.exists():
    sys.exit("run generate_puzzle.py first (no ground_truth.json)")
if not pred_path.exists():
    sys.exit("run driver.py first (no predictions.json)")

GT = json.loads(gt_path.read_text())
P = {f["file"]: f for f in json.loads(pred_path.read_text())["frames"]}
tol = GT["tolerance_px"]


def match(pred, gt):
    used = [False] * len(gt)
    tp, errs = 0, []
    for px, py in pred:
        best, bd = -1, tol + 1
        for j, (gx, gy) in enumerate(gt):
            if used[j]:
                continue
            d = ((px - gx) ** 2 + (py - gy) ** 2) ** 0.5
            if d < bd:
                bd, best = d, j
        if best >= 0:
            used[best] = True
            tp += 1
            errs.append(bd)
    return tp, errs


TP = FP = FN = 0
errs_all = []
count_ok = 0
print(f"{'frame':14}{'L':>5}{'gt':>4}{'pred':>5}{'tp':>4}{'fp':>4}{'fn':>4}")
for f in GT["frames"]:
    g = f["centroids"]
    pf = P.get(f["file"], {})
    pr = [(float(a), float(b)) for a, b in pf.get("centroids", [])]
    tp, errs = match(pr, g)
    fp, fn = len(pr) - tp, len(g) - tp
    TP += tp; FP += fp; FN += fn; errs_all += errs
    count_ok += (pf.get("count") == f["count"])
    print(f'{f["file"]:14}{f["level"]:5.2f}{len(g):4}{len(pr):5}{tp:4}{fp:4}{fn:4}')

rec = TP / (TP + FN) if (TP + FN) else 0
prec = TP / (TP + FP) if (TP + FP) else 0
mean_err = sum(errs_all) / len(errs_all) if errs_all else 0
print(f"\ncount exact: {count_ok}/{len(GT['frames'])} frames")
print(f"localization: TP={TP} FP={FP} FN={FN}  "
      f"recall={rec:.3f} precision={prec:.3f}  meanErr={mean_err:.1f}px (tol={tol})")
ok = (FP == 0 and FN == 0 and count_ok == len(GT["frames"]))
print("VERDICT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
